#include "Transport.h"
#include "ArrangementOps.h"
#include "audio/Engine.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtDebug>
#include <algorithm>
#include <cmath>

namespace sessions {

using audio::Command;

namespace {

// Linear peak -> meter position: -60 dB at the bottom, 0 dB at 95 %.
double meterLevel(float peak) {
    if (peak < 1e-6f) return 0.0;
    return std::clamp((20.0 * std::log10(double(peak)) + 60.0) / 63.15, 0.0, 1.0);
}

double fall(double current, float peak) {
    return std::max(meterLevel(peak), current - 0.025);   // ~0.6 s from full scale to empty
}

double ceilBar(double beats) { return std::max(4.0, std::ceil(beats / 4.0 - 1e-9) * 4.0); }

} // namespace

Transport::Transport(Project* project, audio::Engine* engine, QObject* parent)
    : QObject(parent), m_project(project), m_engine(engine)
{
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &Transport::onTimer);
    m_timer.start();
    // The input opens only while an audio track is armed or monitoring.
    connect(project, &Project::mixerChanged, this, &Transport::updateInputWanted);
    connect(project, &Project::structureChanged, this, &Transport::updateInputWanted);
}

bool Transport::armedAudioTrack() const {
    for (const Track& t : m_project->tracks) if (t.type == TrackType::Audio && t.armed) return true;
    return false;
}

void Transport::updateInputWanted() {
    bool wanted = false;
    for (const Track& t : m_project->tracks)
        if (t.type == TrackType::Audio && (t.armed || t.monitor == Monitor::In)) wanted = true;
    m_engine->setInputWanted(wanted || audioRecording());
}

QString Transport::recordFolder() const {
    QString dir = m_project->recordFolder;
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Sessions/Recorded";
    return dir;
}

QString Transport::recordingPath(const QString& trackName) const {
    const QString dir = recordFolder();
    QDir().mkpath(dir);
    QString name = trackName;
    for (QChar& ch : name) if (!ch.isLetterOrNumber() && ch != ' ' && ch != '-' && ch != '_') ch = '_';
    const QString base = QString("%1 %2").arg(name.trimmed().isEmpty() ? QString("Audio") : name.trimmed(),
                                              QDateTime::currentDateTime().toString("yyyy-MM-dd hhmmss"));
    QString path = QDir(dir).filePath(base + ".wav");
    for (int n = 2; QFileInfo::exists(path); ++n) path = QDir(dir).filePath(QString("%1 %2.wav").arg(base).arg(n));
    return path;
}

// ---- audio recording: the capture stream into WAV files ----------------------------

void Transport::ensureCapture() {
    if (m_capturing) return;
    m_engine->setInputWanted(true);
    m_engine->startCapture();
    m_capturing = true;
    m_capFrame = 0;
    m_wrapBeats = 0;
    m_capHistory.clear();
    m_historyStart = 0;
}

namespace {
// Frames of the capture stream as a take keeps them: stereo, or one input on both sides.
void writeTakeFrames(samples::WavWriter& w, const float* in, qint64 frames, int channel) {
    if (channel == 0) { w.write(in, frames); return; }
    std::vector<float> st(size_t(frames) * 2);
    for (qint64 f = 0; f < frames; ++f) st[size_t(f) * 2] = st[size_t(f) * 2 + 1] = in[size_t(f) * 2 + (channel == 2 ? 1 : 0)];
    w.write(st.data(), frames);
}
void writeSilence(samples::WavWriter& w, qint64 frames) {
    std::vector<float> zero(4096 * 2, 0.0f);
    for (qint64 left = frames; left > 0; left -= 4096) w.write(zero.data(), std::min<qint64>(4096, left));
}
} // namespace

void Transport::LivePeaks::add(const float* in, qint64 frames) {
    for (qint64 f = 0; f < frames; ++f) {
        const float l = in[f * 2], r = in[f * 2 + 1];
        curLo = std::min({curLo, l, r}); curHi = std::max({curHi, l, r});
        if (++curCount == kBlock) { lo.push_back(curLo); hi.push_back(curHi); curLo = 1; curHi = -1; curCount = 0; }
    }
}
void Transport::LivePeaks::addSilence(qint64 frames) {
    for (qint64 f = 0; f < frames; ++f) {
        curLo = std::min(curLo, 0.0f); curHi = std::max(curHi, 0.0f);
        if (++curCount == kBlock) { lo.push_back(curLo); hi.push_back(curHi); curLo = 1; curHi = -1; curCount = 0; }
    }
}
void Transport::LivePeaks::range(qint64 f0, qint64 f1, float& outLo, float& outHi) const {
    outLo = outHi = 0;
    const qint64 n = frames();
    if (f1 <= 0 || f0 >= n) return;
    f0 = std::max<qint64>(0, f0); f1 = std::min(n, std::max(f1, f0 + 1));
    const qint64 b0 = f0 / kBlock, b1 = (f1 + kBlock - 1) / kBlock;
    outLo = 1; outHi = -1;
    for (qint64 b = b0; b < b1; ++b) {
        if (b < qint64(lo.size())) { outLo = std::min(outLo, lo[size_t(b)]); outHi = std::max(outHi, hi[size_t(b)]); }
        else if (curCount > 0) { outLo = std::min(outLo, curLo); outHi = std::max(outHi, curHi); }
    }
    if (outLo > outHi) outLo = outHi = 0;
}

const Transport::LivePeaks* Transport::livePeaks(int trackId) const {
    for (const AudioTake& a : m_audioTakes) if (!a.session && a.trackId == trackId && a.peaks && a.framed) return a.peaks.get();
    return nullptr;
}
const Transport::LivePeaks* Transport::livePeaksFor(const ClipPtr& clip) const {
    if (!clip) return nullptr;
    for (const AudioTake& a : m_audioTakes) if (a.session && a.clip == clip && a.peaks && a.framed) return a.peaks.get();
    return nullptr;
}

void Transport::drainCapture() {
    if (!m_capturing || !m_engine->captureStarted()) return;
    const double sr = m_engine->sampleRate(), fpb = sr * 60.0 / m_project->tempo;
    const double cap0 = m_engine->captureStartBeat();
    // Which stream frames each take covers: its beats, shifted later by the latency it had.
    // (A take can start before the capture did: the frames before it are written as silence.)
    auto settle = [&](AudioTake& a) {
        if (!a.framed && a.startBeat >= 0) {
            a.startFrame = std::llround((a.startBeat + a.linear - cap0) * fpb + a.comp * sr); a.framed = true;
            a.peaks = std::make_shared<LivePeaks>();
            a.peaks->startBeat = a.session ? 0.0 : a.startBeat;
            a.peaks->framesPerBeat = fpb;
        }
        if (!a.stopKnown && a.stopBeat >= 0 && a.framed) {
            a.stopFrame = std::max(a.startFrame, qint64(std::llround((a.stopBeat + a.linear - cap0) * fpb + a.comp * sr)));
            a.stopKnown = true;
        }
    };
    // Writes stream frames [from, to) of `data` (which starts at stream frame dataStart) into a take,
    // with silence for anything it missed before them.
    auto feed = [&](AudioTake& a, const float* data, qint64 dataStart, qint64 from, qint64 to) {
        if (!a.framed || !a.writer) return;
        const qint64 end = a.stopKnown ? std::min(to, a.stopFrame) : to;
        const qint64 next = a.startFrame + a.written;
        if (end <= next) return;
        const qint64 begin = std::max(from, next);
        if (begin > next) { writeSilence(*a.writer, begin - next); if (a.peaks) a.peaks->addSilence(begin - next); a.written += begin - next; }
        if (end > begin) {
            writeTakeFrames(*a.writer, data + (begin - dataStart) * 2, end - begin, a.channel);
            if (a.peaks) a.peaks->add(data + (begin - dataStart) * 2, end - begin);
            a.written += end - begin;
        }
    };
    for (AudioTake& a : m_audioTakes) {
        settle(a);
        // A take that learned its start late picks up what it missed from the recent history.
        if (a.framed && a.startFrame + a.written < m_capFrame && !m_capHistory.empty())
            feed(a, m_capHistory.data(), m_historyStart, std::max(a.startFrame, m_historyStart), m_capFrame);
    }
    float buf[4096 * 2];
    for (int got; (got = m_engine->takeRecorded(buf, 4096)) > 0;) {
        const qint64 f0 = m_capFrame, f1 = m_capFrame + got;
        for (AudioTake& a : m_audioTakes) feed(a, buf, f0, f0, f1);
        m_capHistory.insert(m_capHistory.end(), buf, buf + got * 2);
        const size_t keep = size_t(sr * 2) * 2;   // ~2 s
        if (m_capHistory.size() > keep) {
            const size_t drop = m_capHistory.size() - keep;
            m_capHistory.erase(m_capHistory.begin(), m_capHistory.begin() + qint64(drop));
            m_historyStart += qint64(drop / 2);
        }
        m_capFrame = f1;
    }
    for (int i = int(m_audioTakes.size()) - 1; i >= 0; --i)
        if (m_audioTakes[i].stopKnown && m_capFrame >= m_audioTakes[i].stopFrame) finishAudioTake(i);
    // keep capturing while an Arrangement recording is about to take the input (a count-in, before a punch-in)
    const bool takeComing = m_arrRecord && m_playing && armedAudioTrack();
    if (m_audioTakes.isEmpty() && !takeComing) {
        m_engine->stopCapture();
        m_capturing = false;
        updateInputWanted();
    }
}

// The take has all its audio: close the file and put it in the set (one undo step with its pass).
void Transport::finishAudioTake(int index, ClipPtr* created) {
    AudioTake a = m_audioTakes.takeAt(index);
    if (a.writer) a.writer->close();
    const double length = a.stopBeat - a.startBeat;
    const int ti = m_project->indexOfTrack(a.trackId);
    SamplePtr sample = (a.startBeat >= 0 && length > 1e-3) ? samples::load(a.path) : nullptr;
    const double spb = 60.0 / m_project->tempo;
    if (a.session) {
        if (!a.clip) return;
        EditScope edit(m_project, "Record", recordMergeKey(m_takeCounter));
        if (!sample) {   // never started (stopped before its launch): take the empty clip out again
            if (ti >= 0) for (ClipPtr& c : m_project->tracks[ti].clipSlots) if (c == a.clip) c = nullptr;
            QFile::remove(a.path);
            emit m_project->structureChanged();
        } else {
            a.clip->sample = sample;
            a.clip->warpMarkers = {{0, 0}, {a.clip->length, a.clip->length * spb}};
            emit m_project->clipChanged();
        }
        emit recordingChanged();
        return;
    }
    if (ti < 0 || !sample || length < 1.0 / 16) { QFile::remove(a.path); emit recordingChanged(); return; }
    Track& t = m_project->tracks[ti];
    EditScope edit(m_project, "Arrangement Record", recordMergeKey(m_takeCounter));
    // Loop recording: a later pass of the loop is another take of the clip the first pass made.
    if (ClipPtr run = m_loopClips.value(a.trackId)) {
        for (const ArrangementClip& pl : t.arrangement) {
            if (pl.clip != run) continue;
            const double startIn = a.startBeat - pl.start;   // where this pass begins in the clip
            if (startIn < -1e-6 || startIn > pl.length - 1e-6) break;
            addTake(*run, a.path, sample, startIn, length);
            if (created) *created = run;
            emit m_project->clipChanged();
            emit recordingChanged();
            return;
        }
    }
    // A new clip: over the whole loop when this pass ran to the loop end with more to come (the
    // takes that follow line up with it), else over just what was recorded.
    const bool overLoop = a.wrapped && m_loop;
    const double cStart = overLoop ? std::max(m_loopStart, punchStart()) : a.startBeat;
    const double cLen = overLoop ? std::max(length, std::min(m_loopStart + m_loopLength, punchEnd()) - cStart) : length;
    ClipPtr c = m_project->newClip(t.name, cLen);
    c->samplePath = a.path;
    c->sample = sample;
    c->loop = false;
    c->warpMarkers = {{0, 0}, {cLen, cLen * spb}};
    c->takes.push_back({a.path, sample, "Take 1", std::max(0.0, a.startBeat - cStart)});
    arrangement::clearRange(*m_project, t, cStart, cStart + cLen);
    t.arrangement.push_back({cStart, cLen, 0, c});
    if (overLoop) m_loopClips.insert(a.trackId, c);
    if (created) *created = c;
    emit m_project->structureChanged();
    emit recordingChanged();
}

// Another pass of a loop-recorded clip, beginning `startIn` beats into it. A full pass plays
// everywhere (it's the newest take); a pass cut short plays where it has audio, and the pass
// before it keeps playing where it hasn't.
void Transport::addTake(Clip& c, const QString& path, const SamplePtr& sample, double startIn, double length) {
    const int prev = int(c.takes.size()) - 1;
    if (prev >= 0 && c.comp.isEmpty()) c.comp.push_back({0, c.length, prev});   // what played until now, explicitly
    c.takes.push_back({path, sample, QString("Take %1").arg(c.takes.size() + 1), startIn});
    const int idx = int(c.takes.size()) - 1;
    c.samplePath = path;
    c.sample = sample;
    if (startIn <= 1e-6 && length >= c.length - 1e-3) c.comp.clear();   // a full pass: the newest take everywhere
    else c.useTake(startIn, std::min(c.length, startIn + length), idx);
}

void Transport::send(const Command& c) {
    m_engine->flushPublish();   // the engine must see the clips this command refers to (e.g. one just created)
    m_sentSeq = m_engine->send(c);
}

void Transport::setBeat(double b) {
    finishTakes();
    m_beat = std::max(0.0, b);
    send({Command::Locate, -1, -1, m_beat});
    emit tick();
}

QString Transport::positionText() const {
    if (m_countingIn) {   // the count-in reads -2. 1. 1 … -1. 4. 4, then 1. 1. 1
        const double done = m_countInBars * 4.0 - m_countLeft;
        const int barsLeft = std::max(1, int(std::ceil(m_countLeft / 4 - 1e-9)));
        return QString("-%1. %2. %3").arg(barsLeft).arg(int(std::floor(std::fmod(done, 4.0))) + 1).arg(int(std::floor(std::fmod(done, 1.0) * 4)) + 1);
    }
    int bar  = int(std::floor(m_beat / 4)) + 1;
    int bt   = int(std::floor(std::fmod(m_beat, 4.0))) + 1;
    int six  = int(std::floor(std::fmod(m_beat, 1.0) * 4)) + 1;
    return QString("%1. %2. %3").arg(bar).arg(bt).arg(six);
}

void Transport::setLoopEnabled(bool on) { m_loop = on; sendLoop(); emit stateChanged(); }
void Transport::setLoop(double start, double length) {
    m_loopStart = std::max(0.0, start);
    m_loopLength = std::max(0.25, length);
    sendLoop();
    emit stateChanged();
}
void Transport::sendLoop() { send({Command::Loop, -1, -1, m_loopStart, m_loopLength, m_loop}); }

void Transport::setMetronome(bool on) {
    m_metronome = on;
    send({Command::Metronome, -1, -1, 0, 0, on});
    emit stateChanged();
}

void Transport::setCountInBars(int bars) { m_countInBars = std::clamp(bars, 0, 8); emit stateChanged(); }

void Transport::setPunchIn(bool on) {
    if (on == m_punchIn) return;
    finishTakes();
    m_punchIn = on;
    emit stateChanged();
}

void Transport::setPunchOut(bool on) {
    if (on == m_punchOut) return;
    finishTakes();
    m_punchOut = on;
    emit stateChanged();
}

void Transport::previewNote(int track, int pitch, double velocity) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    send({Command::Preview, m_project->tracks[track].id, -1, double(pitch), velocity});
}

void Transport::noteOn(int track, int pitch, double velocity) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    send({Command::NoteOn, m_project->tracks[track].id, -1, double(pitch), velocity});
    m_held.insert(track << 8 | pitch, {track, m_beat, velocity});
}

void Transport::perform(int track, int kind, double value) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    send({Command::Perform, m_project->tracks[track].id, -1, double(kind), value});
}

void Transport::noteOff(int track, int pitch) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    send({Command::NoteOff, m_project->tracks[track].id, -1, double(pitch), 0});
    if (!m_held.contains(track << 8 | pitch)) return;
    const HeldNote h = m_held.take(track << 8 | pitch);
    if (!m_playing || m_countingIn) return;
    const Track& t = m_project->tracks[track];
    const double length = std::max(1.0 / 16, std::min(m_beat, punchEnd()) - h.beat);

    // Session recording: the note lands in the recording clip, relative to its launch.
    if (m_rec.active && m_rec.trackId == t.id && t.playingSlot == m_rec.scene && t.clipSlots.value(m_rec.scene) == m_rec.clip) {
        const double at = h.beat - t.slotStart;
        if (at >= -0.05) {
            EditScope edit(m_project, "Record", recordMergeKey(m_takeCounter));
            m_rec.clip->notes.push_back({pitch, std::max(0.0, at), length, h.velocity, 1.0});
            emit m_project->clipChanged();
        }
        return;
    }
    // Arrangement recording: armed tracks that aren't playing a Session clip collect notes in a take.
    if (m_arrRecord && t.armed && t.type == TrackType::Midi && t.playingSlot < 0 && inPunch(h.beat)) {
        auto it = std::find_if(m_takes.begin(), m_takes.end(), [&](const Take& k) { return k.keys && k.trackId == t.id; });
        if (it == m_takes.end()) {
            Take k;
            k.trackId = t.id;
            k.keys = true;
            k.source = m_project->newClip(t.name, 4);
            // from the bar the note fell in, or exactly at the punch-in
            k.start = k.end = m_punchIn ? m_loopStart : std::floor(std::max(m_recStart, h.beat) / 4) * 4;
            m_takes.push_back(k);
            it = m_takes.end() - 1;
        }
        if (h.beat >= it->start) it->source->notes.push_back({pitch, h.beat - it->start, length, h.velocity, 1.0});
        emit recordingChanged();
    }
}

void Transport::setStartMarker(double b) {
    m_startMarker = std::max(0.0, b);
    if (!m_playing) setBeat(m_startMarker);
    emit stateChanged();
}

void Transport::play() {
    if (m_playing) return;
    m_beat = m_startMarker;
    send({Command::Locate, -1, -1, m_beat});
    startPlaying();
}

void Transport::continuePlay() {
    if (m_playing) { stop(); return; }
    startPlaying();
}

void Transport::startPlaying() {
    // a recording starting from a standstill gets the count-in
    const bool count = m_countInBars > 0 && (m_arrRecord || m_countInNext);
    m_countInNext = false;
    m_countingIn = count;
    m_countLeft = count ? m_countInBars * 4.0 : 0.0;
    m_playing = true;
    m_recStart = m_beat;
    m_loopClips.clear();   // a new recording run: the first pass makes a new clip
    send({Command::Play, -1, -1, count ? m_countInBars * 4.0 : 0.0});
    // The capture starts with the playhead (or the count-in). After Play, so the engine knows
    // how much count-in is left when it stamps the capture's first frame.
    if (m_arrRecord && armedAudioTrack()) ensureCapture();
    emit stateChanged();
    emit tick();
}

void Transport::stop() {
    m_countingIn = false;
    m_countLeft = 0;
    finishSessionRecord(true);
    finishTakes();
    const bool rewind = !m_playing;
    if (rewind) m_beat = m_startMarker = 0;
    m_playing = false;
    for (Track& t : m_project->tracks) { t.playingSlot = -1; t.queuedSlot = -2; }
    send({Command::Stop, -1, -1, 0, 0, rewind});
    if (m_arrRecord) {   // stopping ends the recording: the record button goes out (0.21)
        m_arrRecord = false;   // (the loop clips stay: the audio takes still in flight land in them)
        emit recordingChanged();
    }
    emit stateChanged();
    emit sessionChanged();
    emit tick();
}

void Transport::togglePlay() {
    if (m_playing) stop(); else play();
}

double Transport::nextQuantPoint(double q) const {
    if (!m_playing || q <= 0) return m_beat;
    return std::ceil(m_beat / q - 1e-9) * q;
}

void Transport::launchSlot(int track, int scene) { launch(track, scene, false); }

void Transport::launch(int track, int scene, bool fromScene, bool immediate) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[track];
    if (scene < 0 || scene >= t.clipSlots.size() || !t.clipSlots[scene]) return;
    const ClipPtr clip = t.clipSlots[scene];
    if (m_rec.active && m_rec.clip == clip) { finishSessionRecord(); return; }   // clicking the recording clip ends the take
    if (!fromScene && clip->launchMode == LaunchMode::Toggle && (t.playingSlot == scene || t.queuedSlot == scene)) {
        stopTrack(track);
        return;
    }
    const double q = immediate ? 0.0 : clip->quantization >= 0 ? clip->quantization : m_quant;
    if (!m_playing) {
        // From a standstill the song starts at the start marker, so the launch must be queued there
        // (0.24). Queued first, it landed where the song had last stopped: stop mid-song, launch a
        // clip, and it waited until the playhead got back to that beat.
        m_beat = m_startMarker;
        send({Command::Locate, -1, -1, m_beat});
    }
    t.queuedSlot = scene;
    t.queuedAt = immediate ? m_beat : nextQuantPoint(q);
    t.overridden = true;
    // The engine quantizes against its own clock, so the launch lands on the beat you hear next
    // (stopped, it lands at once: Engine::quantized returns the beat the song will start at).
    send({Command::Launch, t.id, scene, m_playing && !immediate ? q : 0.0, 0, !fromScene && clip->launchMode == LaunchMode::Repeat});
    if (!m_playing) {
        // Launched from a standstill, the Session takes every track: the Arrangement stays silent
        // (Back to Arrangement hands the tracks back). Launched while the song plays, only this track.
        for (Track& o : m_project->tracks) o.overridden = true;
        send({Command::SessionTakeover});
        play();
    }
    emit sessionChanged();
}

void Transport::releaseSlot(int track, int scene) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[track];
    const ClipPtr clip = t.clipSlots.value(scene);
    if (!clip) return;
    if (clip->launchMode == LaunchMode::Gate && (t.playingSlot == scene || t.queuedSlot == scene)) stopTrack(track);
    else if (clip->launchMode == LaunchMode::Repeat) send({Command::Unrepeat, t.id});
}

void Transport::stopTrack(int track) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[track];
    if (m_rec.active && m_rec.trackId == t.id) finishSessionRecord();
    if (t.playingSlot >= 0 || t.queuedSlot >= 0) { t.queuedSlot = -1; t.queuedAt = nextQuantPoint(m_quant); }
    send({Command::StopTrack, t.id, -1, m_playing ? m_quant : 0.0});
    emit sessionChanged();
}

void Transport::launchScene(int scene) {
    // From a standstill the first launch starts the song; the rest must not then wait for the next
    // quantization point (they'd flash for a bar): every clip in the row starts at once.
    const bool atOnce = !m_playing;
    for (int i = 0; i < m_project->tracks.size(); ++i) {
        Track& t = m_project->tracks[i];
        if (scene < t.clipSlots.size() && t.clipSlots[scene]) launch(i, scene, true, atOnce); else stopTrack(i);
    }
}

void Transport::playClipFrom(const ClipPtr& clip, double clipBeat) {
    if (!clip) return;
    const int track = m_project->trackOfClip(clip);
    if (track < 0 || track >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[track];
    clipBeat = std::max(0.0, clipBeat);
    if (clip->loop && clip->length > 0 && clipBeat >= clip->loopEnd()) clipBeat = clip->loopStart + std::fmod(clipBeat - clip->loopStart, clip->length);
    // on the timeline (and the Session isn't playing this track): go there
    if (!t.overridden || t.playingSlot < 0)
        for (const ArrangementClip& a : t.arrangement)
            if (a.clip == clip) {
                const double at = a.beatOf(clipBeat);
                if (at >= 0) {
                    setStartMarker(at);
                    if (m_playing) setBeat(at);
                    if (t.playingSlot >= 0) stopTrack(track);
                    return;
                }
            }
    const int scene = int(t.clipSlots.indexOf(clip));
    if (scene < 0) return;
    if (t.playingSlot != scene) launch(track, scene, true, true);   // at once, whatever the quantization
    const double elapsed = clipBeat - clip->start;   // beats after a launch at which the clip is at that beat (0.23: before the start marker is fine)
    t.slotStart = m_beat - elapsed;
    send({Command::SeekSlot, t.id, scene, elapsed});
    emit sessionChanged();
}

void Transport::stopAll() { for (int i = 0; i < m_project->tracks.size(); ++i) stopTrack(i); }

void Transport::backToArrangement() {
    for (Track& t : m_project->tracks) { t.playingSlot = -1; t.queuedSlot = -2; t.overridden = false; }
    send({Command::BackToArrangement});
    emit sessionChanged();
}

void Transport::reset() {
    m_rec = {};
    m_takes.clear();
    for (AudioTake& a : m_audioTakes) {   // recordings in progress belong to the old set: drop them
        if (a.writer) a.writer->close();
        QFile::remove(a.path);
    }
    m_audioTakes.clear();
    m_loopClips.clear();
    if (m_capturing) { m_engine->stopCapture(); m_capturing = false; }
    m_held.clear();
    m_arrRecord = false;
    m_playing = false;
    m_beat = m_startMarker = 0;
    for (Track& t : m_project->tracks) { t.playingSlot = -1; t.queuedSlot = -2; t.overridden = false; }
    send({Command::Stop, -1, -1, 0, 0, true});
    send({Command::BackToArrangement});
    emit stateChanged();
    emit sessionChanged();
    emit recordingChanged();
    emit tick();
}

// ---- recording ------------------------------------------------------------

bool Transport::isRecordingSlot(int track, int scene) const {
    const Track* t = m_project->trackAt(track);
    return m_rec.active && t && t->id == m_rec.trackId && scene == m_rec.scene;
}

void Transport::recordSlot(int track, int scene) {
    if (track < 0 || track >= m_project->tracks.size()) return;
    Track& t = m_project->tracks[track];
    if (scene < 0 || scene >= t.clipSlots.size() || t.clipSlots[scene] || t.isGroup()) return;
    finishSessionRecord();
    ++m_takeCounter;
    if (t.type == TrackType::Audio) {   // the input into a new audio clip
        AudioTake a;
        a.trackId = t.id;
        a.session = true;
        a.channel = t.inputChannel;
        a.path = recordingPath(t.name);
        a.writer = std::make_shared<samples::WavWriter>();
        QString error;
        if (!a.writer->open(a.path, int(m_engine->sampleRate()), samples::WavFormat::Float32, &error)) {
            qWarning("Sessions: can't record to %s: %s", qPrintable(a.path), qPrintable(error));
            return;
        }
        {
            EditScope edit(m_project, "Record", recordMergeKey(m_takeCounter));
            a.clip = m_project->newClip(t.name, 8);   // grows a bar at a time while recording
            a.clip->samplePath = a.path;
            a.clip->warpMarkers = {{0, 0}, {8, 8 * 60.0 / m_project->tempo}};
            m_project->tracks[track].clipSlots[scene] = a.clip;
            emit m_project->structureChanged();
        }
        m_engine->setInputWanted(true);   // open, so its latency is known
        a.comp = m_engine->latencySeconds() + m_engine->inputLatencySeconds();
        m_audioTakes.push_back(a);
        m_countInNext = !m_playing;
        launch(track, scene, true);   // starts the transport (and its count-in) when stopped
        ensureCapture();              // after Play: see startPlaying()
        m_rec = {true, t.id, scene, a.clip};
        emit recordingChanged();
        return;
    }
    ClipPtr c;
    {
        EditScope edit(m_project, "Record", recordMergeKey(m_takeCounter));
        c = m_project->newClip(t.name, 8);   // grows a bar at a time while recording
        t.clipSlots[scene] = c;
        emit m_project->structureChanged();
    }
    m_countInNext = !m_playing;   // a recording from a standstill: the count-in first
    launch(track, scene, true);   // before marking it: launching the recording clip ends a take
    m_rec = {true, t.id, scene, c};
    emit recordingChanged();
}

void Transport::finishSessionRecord(bool now) {
    if (!m_rec.active) return;
    const int ti = m_project->indexOfTrack(m_rec.trackId);
    if (ti >= 0 && m_rec.clip) {
        const Track& t = m_project->tracks[ti];
        // ask the engine too: the 60 Hz mirror may not have caught the launch yet
        const auto engineSays = m_engine->trackStatus(ti);
        const bool engineStarted = m_engine->inSync() && engineSays.playingSlot == m_rec.scene;
        const bool started = t.playingSlot == m_rec.scene || engineStarted;
        const double slotStart = t.playingSlot == m_rec.scene ? t.slotStart : engineSays.slotStart;
        const double elapsed = started ? m_beat - slotStart : 0.0;
        EditScope edit(m_project, "Record", recordMergeKey(m_takeCounter));
        m_rec.clip->length = ceilBar(elapsed);   // the take loops from here on
        emit m_project->clipChanged();
        // An audio take keeps recording to the end of that bar (or stops here, when the transport stops).
        for (AudioTake& a : m_audioTakes)
            if (a.session && a.clip == m_rec.clip && a.stopBeat < 0) {
                if (a.startBeat < 0 && started) { a.startBeat = slotStart; a.linear = m_wrapBeats; }
                const double nowIn = m_beat + (m_wrapBeats - a.linear);   // the playhead in the take's beats
                a.stopBeat = a.startBeat < 0 ? 0 : now ? std::max(a.startBeat, nowIn) : a.startBeat + m_rec.clip->length;
                if (a.startBeat < 0) a.startBeat = 0;   // never launched: finishAudioTake takes the clip out
            }
    }
    m_rec = {};
    emit recordingChanged();
}

void Transport::setArrangementRecord(bool on) {
    if (on == m_arrRecord) return;
    if (!on) finishTakes();
    m_arrRecord = on;
    m_recStart = m_beat;
    m_loopClips.clear();
    if (on && m_playing && armedAudioTrack()) ensureCapture();
    ++m_takeCounter;
    emit stateChanged();
    emit recordingChanged();
}

// While Arrangement Record runs, every launched Session clip is written to its track's timeline.
void Transport::updateTakes() {
    if (!m_arrRecord || !m_playing || m_countingIn) return;
    bool changed = false;
    const bool punched = inPunch(m_beat);
    for (int i = 0; i < m_project->tracks.size(); ++i) {
        const Track& t = m_project->tracks[i];
        const ClipPtr playing = t.playingSlot >= 0 ? t.clipSlots.value(t.playingSlot) : nullptr;
        int k = -1;
        for (int j = 0; j < m_takes.size(); ++j) if (!m_takes[j].keys && m_takes[j].trackId == t.id) k = j;
        if (k >= 0 && (m_takes[k].source != playing || m_takes[k].slotStart != t.slotStart || !punched)) {
            m_takes[k].end = std::min(std::max(m_takes[k].end, m_beat), punchEnd());
            finishTake(k); k = -1; changed = true;
        }
        if (!playing || (m_rec.active && m_rec.clip == playing) || !punched) continue;
        if (k < 0) {
            Take take;
            take.trackId = t.id;
            take.source = playing;
            take.slotStart = t.slotStart;
            take.start = take.end = std::max({m_recStart, t.slotStart, punchStart()});
            m_takes.push_back(take);
            k = int(m_takes.size()) - 1;
            changed = true;
        }
        m_takes[k].end = std::max(m_takes[k].end, m_beat);
    }
    // Armed audio tracks that aren't playing a Session clip record the input onto the timeline.
    for (int i = 0; i < m_project->tracks.size(); ++i) {
        const Track& t = m_project->tracks[i];
        const bool wants = t.type == TrackType::Audio && t.armed && t.playingSlot < 0 && punched;
        const int k = int(std::find_if(m_takes.begin(), m_takes.end(), [&](const Take& x) { return x.audio && x.trackId == t.id; }) - m_takes.begin());
        if (k < m_takes.size() && !wants) {
            m_takes[k].end = std::min(std::max(m_takes[k].end, m_beat), punchEnd());
            finishTake(k); changed = true; continue;
        }
        if (k < m_takes.size() || !wants) continue;
        AudioTake a;
        a.trackId = t.id;
        a.channel = t.inputChannel;
        a.path = recordingPath(t.name);
        a.writer = std::make_shared<samples::WavWriter>();
        QString error;
        if (!a.writer->open(a.path, int(m_engine->sampleRate()), samples::WavFormat::Float32, &error)) {
            qWarning("Sessions: can't record to %s: %s", qPrintable(a.path), qPrintable(error));
            continue;
        }
        ensureCapture();
        a.comp = m_engine->latencySeconds() + m_engine->inputLatencySeconds();
        // punch in where recording began (or at the punch-in), or here when the track was armed later
        const double from = std::max(m_recStart, punchStart());
        a.startBeat = m_beat - from < 1.0 ? from : m_beat;
        a.linear = m_wrapBeats;
        m_audioTakes.push_back(a);
        Take take;
        take.trackId = t.id;
        take.audio = true;
        take.start = take.end = a.startBeat;
        m_takes.push_back(take);
        changed = true;
    }
    for (Take& k : m_takes) if (k.keys || k.audio) k.end = std::min(std::max(k.end, m_beat), punchEnd());
    if (changed) emit recordingChanged();
}

void Transport::finishTake(int index, bool wrapped) {
    if (index < 0 || index >= m_takes.size()) return;
    const Take take = m_takes.takeAt(index);
    if (take.audio) {   // its file is finished once the stream has delivered the last frame (drainCapture)
        for (AudioTake& a : m_audioTakes)
            if (!a.session && a.trackId == take.trackId && a.stopBeat < 0) { a.stopBeat = std::max(a.startBeat, take.end); a.wrapped = wrapped; }
        emit recordingChanged();
        return;
    }
    const int ti = m_project->indexOfTrack(take.trackId);
    if (ti < 0 || !take.source || take.end - take.start < 1.0 / 16) return;
    Track& t = m_project->tracks[ti];
    EditScope edit(m_project, "Arrangement Record", recordMergeKey(m_takeCounter));
    ArrangementClip a;
    a.start = take.start;
    if (take.keys) {
        a.length = std::min(ceilBar(take.end - take.start), std::max(1.0 / 16, punchEnd() - take.start));   // not past the punch-out
        take.source->length = a.length;
        a.clip = take.source;
    } else {
        a.length = take.end - take.start;
        a.offset = std::max(0.0, take.source->positionAt(take.start - take.slotStart));   // where the clip was when the take began
        a.clip = m_project->copyClip(take.source);
    }
    arrangement::clearRange(*m_project, t, a.start, a.start + a.length);
    t.arrangement.push_back(a);
    emit m_project->structureChanged();
    emit recordingChanged();
}

void Transport::finishTakes(double endAt) {
    updateTakes();
    if (endAt >= 0) for (Take& k : m_takes) k.end = std::min(std::max(k.end, endAt), punchEnd());   // exactly at the loop end
    while (!m_takes.isEmpty()) finishTake(int(m_takes.size()) - 1, endAt >= 0);
}

void Transport::onTimer() {
    m_engine->collectGarbage();
    bool sessionDirty = false;

    // Only adopt the engine's state once it has applied everything we sent;
    // until then the optimistic UI state from the last click stands.
    if (m_engine->doneSeq() >= m_sentSeq) {
        const bool playing = m_engine->playing();
        double beat = m_engine->beat();
        const double left = m_engine->countInLeft();
        const bool counting = playing && left > 0;
        m_countLeft = left;
        if (counting != m_countingIn) {   // the count-in ended: recording starts now
            m_countingIn = counting;
            emit stateChanged();
        }
        if (playing && !counting) {
            beat -= m_engine->latencySeconds() * m_project->tempo / 60.0;   // show what's coming out of the speakers
            if (m_loop && beat < m_loopStart && m_engine->beat() >= m_loopStart) beat += m_loopLength;
            beat = std::max(0.0, beat);
        }
        if (playing && m_loop && beat < m_beat - 0.5) {   // the loop wrapped: this pass's takes end at the loop end, the next begins at its start
            finishTakes(m_loopStart + m_loopLength);
            m_wrapBeats += m_loopLength;
            m_recStart = m_loopStart;
        }
        m_beat = beat;
        if (playing != m_playing) { m_playing = playing; emit stateChanged(); }
        if (m_engine->inSync()) {
            for (int i = 0; i < m_project->tracks.size(); ++i) {
                Track& t = m_project->tracks[i];
                const auto s = m_engine->trackStatus(i);
                if (s.playingSlot != t.playingSlot || s.queuedSlot != t.queuedSlot || s.overridden != t.overridden) sessionDirty = true;
                t.playingSlot = s.playingSlot;
                t.queuedSlot = s.queuedSlot;
                t.slotStart = s.slotStart;
                t.queuedAt = s.queuedAt;
                t.overridden = s.overridden;
            }
        }
    }

    // A recording clip grows a bar ahead of the playhead so it never loops mid-take.
    if (m_rec.active) {
        const int ti = m_project->indexOfTrack(m_rec.trackId);
        if (ti < 0 || !m_project->tracks[ti].clipSlots.contains(m_rec.clip)) {
            for (AudioTake& a : m_audioTakes)   // the clip was deleted: drop its audio
                if (a.session && a.clip == m_rec.clip && a.stopBeat < 0) { a.startBeat = std::max(0.0, a.startBeat); a.stopBeat = a.startBeat; }
            m_rec = {};
            emit recordingChanged();
        } else if (m_project->tracks[ti].playingSlot == m_rec.scene) {
            const double want = ceilBar(m_beat - m_project->tracks[ti].slotStart) + 4;
            if (m_rec.clip->length < want) { m_rec.clip->length = want; emit m_project->clipChanged(); }
            for (AudioTake& a : m_audioTakes)   // the clip has started: that's where its audio begins
                if (a.session && a.clip == m_rec.clip && a.startBeat < 0) { a.startBeat = m_project->tracks[ti].slotStart; a.linear = m_wrapBeats; }
        }
    }
    updateTakes();
    drainCapture();
    for (int i = 0; i < m_project->tracks.size(); ++i)   // input meters of audio tracks
        if (m_project->tracks[i].type == TrackType::Audio)
            m_project->tracks[i].inputMeter = fall(m_project->tracks[i].inputMeter, m_engine->takeInputPeak(i));

    if (m_engine->inSync()) {
        for (int i = 0; i < m_project->tracks.size(); ++i)
            m_project->tracks[i].meter = fall(m_project->tracks[i].meter, m_engine->takeTrackPeak(i));
        for (int i = 0; i < m_project->returns.size(); ++i)
            m_project->returns[i].meter = fall(m_project->returns[i].meter, m_engine->takeReturnPeak(i));
    }
    m_project->mainMeter = fall(m_project->mainMeter, m_engine->takeMainPeak());

    if (sessionDirty) emit sessionChanged();
    emit tick();
}

} // namespace sessions
