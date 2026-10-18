#pragma once
// Transport: the UI's handle on playback. Actions update the model straight
// away (so views react on the same click) and are sent to the audio engine,
// which owns the real clock. A 60 Hz timer mirrors the engine's playhead,
// launch state and meters back into the model once it has caught up with
// every command sent.
//
// Transport also does the recording, on the UI thread: notes played on the
// computer keyboard into a Session slot or onto the timeline, launched Session
// clips onto the timeline while Arrangement Record is on, and the audio input
// into armed audio tracks (Session slots or the timeline). Audio takes stream
// from the engine's capture ring into 32-bit float WAVs in Project::recordFolder,
// shifted earlier by the output + input latency so they line up with what you heard.
// The input is opened only while an audio track is armed or set to Monitor In.
//
// Loop recording onto the timeline keeps every pass: the first pass makes one clip
// over the loop, and each later pass is added to it as a take (Clip::takes); the
// newest plays until you comp them in the Takes view. A pass cut short by stopping
// is kept too, playing where it has audio and the pass before it elsewhere.

#include "Model.h"
#include "Sample.h"
#include <QObject>
#include <QTimer>
#include <QHash>

namespace sessions {

namespace audio { class Engine; struct Command; }

class Transport : public QObject {
    Q_OBJECT
public:
    Transport(Project* project, audio::Engine* engine, QObject* parent = nullptr);

    audio::Engine* engine() const { return m_engine; }

    bool   playing() const { return m_playing; }
    double beat() const { return m_beat; }        // what you hear: compensated for output latency
    void   setBeat(double b);                     // jump the playhead
    // Where Play starts (the insert marker): set by clicking the Arrangement (a clip
    // starts at its beginning), the ruler or a locator. Stopping twice resets it to 1.1.1.
    double startMarker() const { return m_startMarker; }
    void   setStartMarker(double b);              // also moves the playhead while stopped

    double launchQuantization() const { return m_quant; }  // beats; 0 = none
    void   setLaunchQuantization(double q) { m_quant = q; emit stateChanged(); }

    bool   loopEnabled() const { return m_loop; }
    void   setLoopEnabled(bool on);
    double loopStart() const { return m_loopStart; }
    double loopLength() const { return m_loopLength; }
    void   setLoop(double start, double length);

    bool   metronome() const { return m_metronome; }
    void   setMetronome(bool on);

    // Count-in: when Play starts a recording (Arrangement Record on, or a Session slot's ●
    // with the transport stopped), this many bars of click come first. 0 = none.
    int    countInBars() const { return m_countInBars; }
    void   setCountInBars(int bars);
    bool   countingIn() const { return m_countingIn; }
    // Punch in / out: Arrangement recording only between the loop's start and / or end.
    bool   punchIn() const { return m_punchIn; }
    bool   punchOut() const { return m_punchOut; }
    void   setPunchIn(bool on);
    void   setPunchOut(bool on);

    QString positionText() const;

    // Audition a note through a track's instrument (note editor clicks).
    void previewNote(int track, int pitch, double velocity = 0.8);
    // Live notes (computer MIDI keyboard): held until noteOff, and recorded when a recording is running.
    void noteOn(int track, int pitch, double velocity);
    void noteOff(int track, int pitch);
    // Performance controls from a keyboard, for a track's instruments: kind 0 = pitch bend (-1..1),
    // 1 = the mod wheel (0..1), 2 = channel pressure (0..1). Not recorded.
    void perform(int track, int kind, double value);

    // ---- recording
    bool arrangementRecord() const { return m_arrRecord; }
    void setArrangementRecord(bool on);
    void recordSlot(int track, int scene);           // start recording a new clip into an empty slot (armed track)
    bool sessionRecording() const { return m_rec.active; }
    bool isRecordingSlot(int track, int scene) const;
    struct Take {                                     // timeline recording in progress
        int     trackId = 0;
        bool    keys = false;   // notes from the keyboard (else: a launched Session clip)
        bool    audio = false;  // the audio input
        ClipPtr source;         // the Session clip being recorded, or the clip notes go into
        double  slotStart = 0, start = 0, end = 0;
    };
    const QVector<Take>& takes() const { return m_takes; }
    bool audioRecording() const { return !m_audioTakes.isEmpty(); }
    // Where a new recording on this track goes (in Project::recordFolder, or ~/Music/Sessions/Recorded).
    QString recordingPath(const QString& trackName) const;
    QString recordFolder() const;   // that folder (edited samples go there too)
    // The waveform of an audio take still recording (0.22): min / max of the input per kBlock
    // frames from the take's first frame, so the views can draw it as it comes in. `startBeat` is
    // where the take begins on the timeline (a Session take: 0, clip time); framesPerBeat at the
    // set's tempo. nullptr when nothing is recording there.
    struct LivePeaks {
        static constexpr int kBlock = 256;
        double startBeat = 0, framesPerBeat = 1;
        std::vector<float> lo, hi;   // one pair per finished block
        float curLo = 1, curHi = -1; int curCount = 0;   // the block being filled
        void add(const float* interleaved, qint64 frames);
        void addSilence(qint64 frames);
        qint64 frames() const { return qint64(lo.size()) * kBlock + curCount; }
        void range(qint64 f0, qint64 f1, float& outLo, float& outHi) const;   // over [f0, f1) frames
    };
    const LivePeaks* livePeaks(int trackId) const;              // a timeline take on this track
    const LivePeaks* livePeaksFor(const ClipPtr& clip) const;   // a Session clip being recorded

public slots:
    void play();            // from the start marker
    void stop();            // a second stop while stopped returns to 1.1.1
    void togglePlay();      // Space: play from the start marker, or stop
    void continuePlay();    // Shift+Space: play on from where it stopped, or stop
    void launchSlot(int track, int scene);   // mouse down on a clip: honours its launch mode
    void releaseSlot(int track, int scene);  // mouse up: Gate stops, Repeat stops repeating
    void stopTrack(int track);
    void launchScene(int scene);   // every clip in the row together: from a standstill at once, else all on the same quantization point
    // Plays `clip` from `clipBeat` (a click in the sample editor, 0.21): a clip on the timeline moves the
    // playhead (and the start marker) there; a Session clip is launched (if it isn't playing) and set there.
    void playClipFrom(const std::shared_ptr<Clip>& clip, double clipBeat);
    void stopAll();
    void backToArrangement();
    // Before the whole set is replaced (New / Open): stop and rewind, hand back to the
    // Arrangement, and drop recordings in progress without writing them into the old set.
    void reset();

signals:
    void tick();            // ~60 Hz
    void stateChanged();    // play/stop/loop/metronome/record
    void sessionChanged();  // a queued launch resolved
    void recordingChanged();

private:
    void   send(const audio::Command& c);
    void   startPlaying();   // from wherever the playhead is
    void   sendLoop();
    void   launch(int track, int scene, bool fromScene, bool immediate = false);   // immediate: no quantization (a scene from a standstill)
    double nextQuantPoint(double q) const;
    void   onTimer();
    void   updateTakes();
    void   finishTake(int index, bool wrapped = false);   // `wrapped`: closed by the loop wrapping (another pass follows)
    void   finishTakes(double endAt = -1);                 // `endAt` >= 0: the loop wrapped there; every take ends exactly there
    // Ends the Session recording: at the next bar of the take (clicking the clip), or right
    // away (`now`: the transport stopped).
    void   finishSessionRecord(bool now = false);
    // audio recording
    void   updateInputWanted();
    bool   armedAudioTrack() const;
    void   ensureCapture();
    void   drainCapture();
    void   finishAudioTake(int index, ClipPtr* createdClip = nullptr);
    void   addTake(Clip& c, const QString& path, const SamplePtr& sample, double startIn, double length);   // another pass of a loop-recorded clip

    Project*       m_project;
    audio::Engine* m_engine;
    QTimer         m_timer;
    quint64        m_sentSeq   = 0;
    bool           m_playing   = false;
    double         m_beat      = 0.0;
    double         m_startMarker = 0.0;
    double         m_quant     = 4.0;
    bool           m_loop      = false;
    double         m_loopStart = 0.0, m_loopLength = 16.0;
    bool           m_metronome = false;
    int            m_countInBars = 0;
    bool           m_countingIn = false, m_countInNext = false;
    double         m_countLeft = 0;
    bool           m_punchIn = false, m_punchOut = false;
    bool   inPunch(double beat) const { return (!m_punchIn || beat >= m_loopStart - 1e-9) && (!m_punchOut || beat < m_loopStart + m_loopLength - 1e-9); }
    double punchStart() const { return m_punchIn ? m_loopStart : -1e18; }
    double punchEnd() const { return m_punchOut ? m_loopStart + m_loopLength : 1e18; }

    // recording
    struct HeldNote { int track; double beat; double velocity; };
    QHash<int, HeldNote> m_held;      // (track << 8 | pitch) -> where it started
    struct SessionRec { bool active = false; int trackId = 0; int scene = -1; ClipPtr clip; } m_rec;
    bool           m_arrRecord = false;
    double         m_recStart  = 0;
    QVector<Take>  m_takes;
    int            m_takeCounter = 0;   // undo merge key for one recording pass

    struct AudioTake {
        int     trackId = 0;
        bool    session = false;       // into a Session slot (with m_rec), else onto the timeline
        int     channel = 0;           // the track's input channel when it started
        QString path;
        std::shared_ptr<samples::WavWriter> writer;
        ClipPtr clip;                  // session: the clip in the slot
        double  startBeat = -1;        // as heard; -1 while a session clip waits for its launch
        double  stopBeat = -1;         // set when it's told to end
        // Beats wrap at the loop end while the capture's frames run on: `linear` is what had wrapped
        // (m_wrapBeats) when startBeat was set, so startBeat + linear counts from the capture's start.
        double  linear = 0;
        bool    wrapped = false;       // ended by the loop wrapping: another pass of the same loop follows
        double  comp = 0;              // output + input latency, seconds
        qint64  startFrame = 0, stopFrame = 0;     // in the capture stream (startFrame < 0: before the capture began)
        bool    framed = false, stopKnown = false; // startFrame / stopFrame worked out
        qint64  written = 0;
        std::shared_ptr<LivePeaks> peaks;   // what's been written, for drawing while it records
    };
    QVector<AudioTake> m_audioTakes;
    bool           m_capturing = false;
    double         m_wrapBeats = 0;         // beats the loop has wrapped since the capture began
    QHash<int, ClipPtr> m_loopClips;        // per track id: the clip this Arrangement loop recording adds its passes to
    qint64         m_capFrame = 0;          // capture stream frames read so far
    std::vector<float> m_capHistory;        // the last ~2 s read, for takes that learn their start late
    qint64         m_historyStart = 0;      // stream frame of m_capHistory[0]
};

} // namespace sessions
