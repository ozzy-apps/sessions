#pragma once
// Streaming long drum recordings from disk. A recorded library keeps each long recording's first
// half second in memory (Layer::mono / right: the head) and leaves the rest on disk. When a voice
// starts a streamed recording it claims a slot from one shared pool; the streamer thread opens the
// file and fills the slot's ring with the frames after the head, ahead of playback, and the voice
// reads them there once it has played the head. The audio thread never blocks, allocates or
// touches a file: if the ring hasn't caught up (a very slow disk) the voice fades out.
// Without a running sound card (exports, tests: rendering faster than real time) it waits instead.
//
// Slot protocol (all lock-free). The voice that claimed a slot owns it until it releases it; only
// the owner changes `gen`. Claiming sets `read` to 0, the source, then bumps `gen` (release). The
// streamer sees a new gen, resets `written`, acknowledges it (`ackGen`), then writes frames and
// publishes `written` (release). Frames count from the end of the head; the ring holds frames
// [written - kStreamRing, written) and the streamer stays kStreamRing ahead of `read`.

#include <QString>
#include <QtGlobal>
#include <atomic>
#include <cstdint>
#include <memory>

namespace sessions::audio::drums {

struct Layer;
constexpr int    kStreamRing = 16384;   // frames a slot buffers ahead (stereo)
constexpr int    kStreamSlots = 512;    // shared by every Drum Studio (64 MB, made with the first streamed library)
constexpr double kHeadSeconds = 0.5;    // what stays in memory of a streamed recording
constexpr double kStreamAbove = 0.75;   // recordings shorter than this (seconds) stay whole

struct StreamSlot {
    std::atomic<bool>     used{false};
    std::atomic<int>      source{-1};
    std::atomic<uint32_t> gen{0}, ackGen{0}, failedGen{~0u};
    std::atomic<qint64>   written{0}, read{0};
    float ring[2][kStreamRing];
    // Frames after the head that are in the ring (0 until the streamer has taken the claim).
    qint64 available() const {
        return ackGen.load(std::memory_order_acquire) == gen.load(std::memory_order_relaxed) ? written.load(std::memory_order_acquire) : 0;
    }
    // The file couldn't be read (or ended early): nothing more will arrive.
    bool failed() const { return failedGen.load(std::memory_order_acquire) == gen.load(std::memory_order_relaxed); }
};

namespace streams {
// ---- audio thread (any thread that renders)
int         claim(int source);   // a slot streaming `source` from the end of its head; -1: none free
void        release(int slot);   // -1 is ignored
StreamSlot& slot(int i);
void        consumed(int slot, qint64 frames);   // the voice won't need frames before this again
int         inUse();                             // slots claimed now (diagnostics, tests)
}

// ---- the decode side (any thread)
bool   streamingEnabled();   // Preferences "drums/stream" (on to begin with)
// A streamed recording: its file (and, for a pair of mono files, the right one), where the head
// ends and how long it is. The same files give the same id. The first one makes the slot pool.
int    registerStream(const QString& path, const QString& rightPath, int channels, qint64 head, qint64 frames, double rate);
// A streamed recording decoded whole, for drawing (UI thread; the file is read now).
std::shared_ptr<const Layer> wholeRecording(const Layer& layer);

// ---- libsndfile, shared with DrumSamples
struct SndInfo { int64_t frames; int samplerate, channels, format, sections, seekable; };
struct SndApi {
    void*   (*open)(const char*, int, SndInfo*) = nullptr;
    int64_t (*readf)(void*, float*, int64_t) = nullptr;
    int64_t (*seek)(void*, int64_t, int) = nullptr;
    int     (*close)(void*) = nullptr;
    bool ok() const { return open && readf && seek && close; }
};
const SndApi& sndfile();

// ---- diagnostics and tests
qint64 streamUnderruns();                 // voices that ran out of streamed frames (real time)
void   noteUnderrun();                    // audio thread
void   setStreamDelayForTests(int ms);    // slows each chunk the streamer reads

}
