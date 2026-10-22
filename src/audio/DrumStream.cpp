#include "DrumStream.h"
#include "DrumSamples.h"
#include <QFile>
#include <QHash>
#include <QLibrary>
#include <QMutex>
#include <QSettings>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace sessions::audio::drums {

const SndApi& sndfile() {
    static const SndApi api = [] {
        static QLibrary lib("libsndfile.so.1");
        SndApi a;
        a.open = reinterpret_cast<void* (*)(const char*, int, SndInfo*)>(lib.resolve("sf_open"));
        a.readf = reinterpret_cast<int64_t (*)(void*, float*, int64_t)>(lib.resolve("sf_readf_float"));
        a.seek = reinterpret_cast<int64_t (*)(void*, int64_t, int)>(lib.resolve("sf_seek"));
        a.close = reinterpret_cast<int (*)(void*)>(lib.resolve("sf_close"));
        return a;
    }();
    return api;
}

namespace {
constexpr int kChunk = 4096;          // frames the streamer reads at a time
constexpr int kSfmRead = 0x10;

struct Source { QByteArray path, right; int channels = 1; qint64 head = 0, frames = 0; double rate = 48000; };

// The streamer's own state for a slot: the files it has open and how far it has read.
struct StreamState {
    uint32_t gen = 0;
    int      source = -1;
    Source   info;
    void*    left = nullptr;
    void*    right = nullptr;
    qint64   written = 0;
    bool     failed = false;
};

// Everything the streamer shares. `sources` grows only (an id stays valid). The pool is made once,
// with the first streamed recording, and lives until the program ends (after the thread stops).
struct Registry {
    QMutex sourcesMutex;
    std::vector<Source> sources;
    QHash<QByteArray, int> ids;
    std::once_flag poolOnce;
    std::unique_ptr<StreamSlot[]> slotStore;
    std::unique_ptr<StreamState[]> states;
    std::atomic<StreamSlot*> pool{nullptr};
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<int> delayMs{0};
    std::atomic<qint64> underruns{0};
    ~Registry();
};
Registry& registry() { static Registry r; return r; }

void closeFiles(StreamState& st) {
    const SndApi& sf = sndfile();
    if (st.left) sf.close(st.left);
    if (st.right) sf.close(st.right);
    st.left = st.right = nullptr;
}

Registry::~Registry() {
    stop = true;
    if (thread.joinable()) thread.join();
    if (states) for (int i = 0; i < kStreamSlots; ++i) closeFiles(states[size_t(i)]);
}

bool openFiles(StreamState& st) {
    const SndApi& sf = sndfile();
    if (!sf.ok()) return false;
    SndInfo info{};
    st.left = sf.open(st.info.path.constData(), kSfmRead, &info);
    if (!st.left || sf.seek(st.left, st.info.head, 0) < 0) return false;
    if (!st.info.right.isEmpty()) {
        SndInfo rinfo{};
        st.right = sf.open(st.info.right.constData(), kSfmRead, &rinfo);
        if (!st.right || sf.seek(st.right, st.info.head, 0) < 0) return false;
    }
    return true;
}

void fail(StreamSlot& s, StreamState& st) {
    st.failed = true;
    closeFiles(st);
    s.failedGen.store(st.gen, std::memory_order_release);
}

// One step for one slot: follow a new generation, or read the next chunk. True when it did work.
bool service(StreamSlot& s, StreamState& st, std::vector<float>& buf, std::vector<float>& rbuf) {
    const uint32_t g = s.gen.load(std::memory_order_acquire);
    if (g != st.gen) {
        closeFiles(st);
        st.gen = g; st.written = 0; st.failed = false;
        st.source = s.source.load(std::memory_order_acquire);
        s.written.store(0, std::memory_order_relaxed);
        s.ackGen.store(g, std::memory_order_release);
        if (st.source < 0) return true;
        {
            auto& r = registry();
            QMutexLocker lock(&r.sourcesMutex);
            if (st.source >= int(r.sources.size())) { fail(s, st); return true; }
            st.info = r.sources[size_t(st.source)];
        }
        if (!openFiles(st)) fail(s, st);
        return true;
    }
    if (st.source < 0 || st.failed || !st.left) return false;
    const qint64 total = st.info.frames - st.info.head;
    if (st.written >= total) { closeFiles(st); return false; }   // all there: nothing more to read
    const qint64 room = s.read.load(std::memory_order_acquire) + kStreamRing - st.written;
    const qint64 n = std::min<qint64>({room, total - st.written, kChunk});
    if (n <= 0 || n < std::min<qint64>(1024, total - st.written)) return false;   // a worthwhile chunk doesn't fit yet
    if (const int delay = registry().delayMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    const SndApi& sf = sndfile();
    const int channels = std::clamp(st.info.channels, 1, 2);
    // A pair of mono files may differ in length: the shorter one ends in silence, as when decoded whole.
    const int64_t gl = std::max<int64_t>(0, sf.readf(st.left, buf.data(), n));
    const int64_t gr = st.right ? std::max<int64_t>(0, sf.readf(st.right, rbuf.data(), n)) : 0;
    const int64_t got = std::max(gl, gr);
    if (got <= 0) { fail(s, st); return true; }   // shorter than it said: the voice runs out and fades
    for (int64_t k = 0; k < got; ++k) {
        const size_t at = size_t((st.written + k) % kStreamRing);
        const float l = k < gl ? buf[size_t(k * channels)] : 0.f;
        const float r = st.right ? (k < gr ? rbuf[size_t(k)] : 0.f) : channels == 2 ? (k < gl ? buf[size_t(k * 2 + 1)] : 0.f) : l;
        s.ring[0][at] = std::isfinite(l) ? l : 0.f;
        s.ring[1][at] = std::isfinite(r) ? r : 0.f;
    }
    st.written += got;
    s.written.store(st.written, std::memory_order_release);
    return true;
}

void run() {
    auto& r = registry();
    StreamSlot* all = r.pool.load(std::memory_order_acquire);
    std::vector<float> buf(size_t(kChunk) * 2);
    std::vector<float> rbuf(static_cast<size_t>(kChunk));
    while (!r.stop.load()) {
        bool busy = false;
        for (int i = 0; i < kStreamSlots; ++i) busy |= service(all[i], r.states[size_t(i)], buf, rbuf);
        if (!busy) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void makePool() {
    auto& r = registry();
    std::call_once(r.poolOnce, [&r] {
        r.slotStore.reset(new StreamSlot[kStreamSlots]);
        r.states.reset(new StreamState[kStreamSlots]);
        r.pool.store(r.slotStore.get(), std::memory_order_release);
        r.thread = std::thread(run);
    });
}
}

namespace streams {
int claim(int source) {
    StreamSlot* pool = registry().pool.load(std::memory_order_acquire);
    if (source < 0 || !pool) return -1;
    for (int i = 0; i < kStreamSlots; ++i) {
        StreamSlot& s = pool[i];
        if (s.used.load(std::memory_order_relaxed) || s.used.exchange(true, std::memory_order_acquire)) continue;
        s.read.store(0, std::memory_order_relaxed);
        s.source.store(source, std::memory_order_relaxed);
        s.gen.fetch_add(1, std::memory_order_release);
        return i;
    }
    return -1;
}

void release(int i) {
    StreamSlot* pool = registry().pool.load(std::memory_order_acquire);
    if (i < 0 || i >= kStreamSlots || !pool) return;
    StreamSlot& s = pool[i];
    s.source.store(-1, std::memory_order_relaxed);
    s.gen.fetch_add(1, std::memory_order_release);   // the streamer closes the file
    s.used.store(false, std::memory_order_release);
}

StreamSlot& slot(int i) { return registry().pool.load(std::memory_order_acquire)[i]; }

void consumed(int i, qint64 frames) {
    if (i >= 0 && i < kStreamSlots) slot(i).read.store(std::max<qint64>(0, frames), std::memory_order_release);
}

int inUse() {
    StreamSlot* pool = registry().pool.load(std::memory_order_acquire);
    int n = 0;
    if (pool) for (int i = 0; i < kStreamSlots; ++i) n += pool[i].used.load(std::memory_order_relaxed);
    return n;
}
}

bool streamingEnabled() { return QSettings("Sessions", "Sessions").value("drums/stream", true).toBool(); }

int registerStream(const QString& path, const QString& rightPath, int channels, qint64 head, qint64 frames, double rate) {
    makePool();   // before any layer that needs it reaches a voice
    auto& r = registry();
    const QByteArray left = QFile::encodeName(path), right = QFile::encodeName(rightPath);
    const QByteArray key = left + '\n' + right;
    QMutexLocker lock(&r.sourcesMutex);
    if (const auto it = r.ids.constFind(key); it != r.ids.constEnd()) {
        Source& s = r.sources[size_t(*it)];
        s.channels = channels; s.head = head; s.frames = frames; s.rate = rate;   // a newer read of the same files
        return *it;
    }
    r.sources.push_back({left, right, channels, head, frames, rate});
    const int id = int(r.sources.size()) - 1;
    r.ids.insert(key, id);
    return id;
}

std::shared_ptr<const Layer> wholeRecording(const Layer& layer) {
    auto out = std::make_shared<Layer>();
    if (layer.stream < 0) { *out = layer; return out; }
    Source src;
    {
        auto& r = registry();
        QMutexLocker lock(&r.sourcesMutex);
        if (layer.stream >= int(r.sources.size())) { *out = layer; return out; }
        src = r.sources[size_t(layer.stream)];
    }
    const SndApi& sf = sndfile();
    out->rate = layer.rate;
    auto readAll = [&](const QByteArray& path, std::vector<float>* left, std::vector<float>* right) {
        SndInfo info{};
        void* f = sf.ok() ? sf.open(path.constData(), kSfmRead, &info) : nullptr;
        if (!f || info.channels < 1 || info.channels > 2 || info.frames < 1) { if (f) sf.close(f); return false; }
        std::vector<float> data(size_t(info.frames) * size_t(info.channels));
        const int64_t got = sf.readf(f, data.data(), info.frames);
        sf.close(f);
        if (got <= 0) return false;
        left->resize(size_t(got));
        if (right && info.channels == 2) right->resize(size_t(got));
        for (int64_t i = 0; i < got; ++i) {
            const float l = data[size_t(i * info.channels)];
            (*left)[size_t(i)] = std::isfinite(l) ? l : 0.f;
            if (right && info.channels == 2) { const float r = data[size_t(i * 2 + 1)]; (*right)[size_t(i)] = std::isfinite(r) ? r : 0.f; }
        }
        return true;
    };
    if (!readAll(src.path, &out->mono, src.right.isEmpty() ? &out->right : nullptr)) { *out = layer; return out; }
    if (!src.right.isEmpty()) readAll(src.right, &out->right, nullptr);
    if (!out->right.empty()) { const size_t n = std::max(out->mono.size(), out->right.size()); out->mono.resize(n); out->right.resize(n); }
    return out;
}

qint64 streamUnderruns() { return registry().underruns.load(); }
void noteUnderrun() { registry().underruns.fetch_add(1, std::memory_order_relaxed); }
void setStreamDelayForTests(int ms) { registry().delayMs = ms; }

}
