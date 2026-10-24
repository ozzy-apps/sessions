#include "DeviceKit.h"
#include "ProductionEffects.h"
#include "Resample.h"
#include "core/Sample.h"

namespace sessions::audio {
using namespace kit;
namespace {
class Looper final : public DeviceProcessor {
    float sr;
    int capacity, length = 0, recorded = 0, targetLength = 0, lastRequested = 0, state = 0, wait = 0, undoAt = 0;
    double position = 0;
    bool lastUndo = false, lastClear = false, undoSelected = false, undoAvailable = false;
    std::vector<float> data, backup;
    std::vector<uint32_t> written, backed;
    uint32_t generation = 1, pass = 1;
    const SampleData* loaded = nullptr;
    bool useLoaded = false;
    Smoothed output;
    int lastStateParam = 0;
    bool triggers[4] = {}, dirty = false;
    std::atomic<DeviceCapture*> requested{nullptr};
    DeviceCapture* capture = nullptr;
    int captureAt = 0;
    uint32_t captureRevision = 0, revision = 0;
    float base(const SampleData* sample, int i, int c) const {
        if (!useLoaded || !sample)
            return 0;
        return resample::read(c ? sample->right : sample->left, i * sample->rate / sr, sample->rate / sr);
    }
    float current(const SampleData* sample, int i, int c) const {
        if (i < 0 || i >= capacity)
            return 0;
        if (undoSelected && backed[i] == pass)
            return backup[size_t(i) * 2 + c];
        return written[i] == generation ? data[size_t(i) * 2 + c] : base(sample, i, c);
    }
    void invalidateCapture() {
        if (capture) {
            capture->state.store(-1, std::memory_order_release);
            capture = nullptr;
        }
        if (auto* request = requested.exchange(nullptr, std::memory_order_acq_rel))
            request->state.store(-1, std::memory_order_release);
    }

  public:
    explicit Looper(float rate)
        : sr(rate), capacity(int(rate * 120)), data(size_t(capacity) * 2), backup(size_t(capacity) * 2),
          written(capacity), backed(capacity) {}
    ~Looper() override {
        invalidateCapture();
    }
    bool requestCapture(DeviceCapture* c) override {
        DeviceCapture* empty = nullptr;
        return c && requested.compare_exchange_strong(empty, c, std::memory_order_release, std::memory_order_relaxed);
    }
    // Captures are retained by the caller until completion. Destruction of the UI never frees a pending request.
    void process(float* l, float* r, int n, const float* p, const Context& ctx) override {
        using namespace looper;
        const T t = T::Looper;
        int stateParam = pc(t, p, State);
        int request = stateParam == lastStateParam ? lastRequested : stateParam;
        lastStateParam = stateParam;
        const int commands[] = {1, 2, 0, 3};
        for (int k = 0; k < 4; ++k) {
            bool trigger = pb(p, RecordTrigger + k);
            if (trigger != triggers[k]) {
                request = commands[k];
                triggers[k] = trigger;
                if (k == 0)
                    lastRequested = -1;
            }
        }
        bool undo = pb(p, Undo), clear = pb(p, Clear);
        if (ctx.sample != loaded) {
            invalidateCapture();
            loaded = ctx.sample;
            useLoaded = loaded != nullptr;
            length = loaded ? std::min(capacity, int(loaded->seconds() * sr)) : 0;
            position = length > 0 ? std::fmod(position, length) : 0;
            ++generation;
            undoAvailable = undoSelected = false;
            dirty = false;
            ++revision;
        }
        if (clear != lastClear) {
            invalidateCapture();
            length = recorded = 0;
            dirty = false;
            position = 0;
            state = 0;
            useLoaded = false;
            ++generation;
            undoAvailable = undoSelected = false;
            ++revision;
            lastClear = clear;
        }
        if (undo != lastUndo) {
            if (undoAvailable && !undoSelected) {
                request = 2;
                invalidateCapture();
                undoSelected = true;
                undoAt = 0;
                ++revision;
            }
            lastUndo = undo;
        }
        if (undoSelected) {
            const int end = std::min(length, undoAt + 8192);
            for (int i = undoAt; i < end; ++i) {
                float a = current(ctx.sample, i, 0), b = current(ctx.sample, i, 1);
                data[size_t(i) * 2] = a;
                data[size_t(i) * 2 + 1] = b;
                written[i] = generation;
                backed[i] = 0;
            }
            undoAt = end;
            if (undoAt == length) {
                undoSelected = false;
                undoAvailable = false;
            }
        }
        if (undoSelected && request == 3)
            request = 2;
        if (request != lastRequested) {
            invalidateCapture();
            if (state == 1 && recorded > 0) {
                length = recorded;
                position = 0;
                ++revision;
            }
            state = request;
            lastRequested = request;
            if (state == 1) {
                dirty = true;
                ++generation;
                useLoaded = false;
                recorded = length = 0;
                position = 0;
                undoAvailable = false;
                targetLength = capacity;
                double beats = pv(t, p, Bars);
                if (beats > 0)
                    targetLength = std::clamp(int(std::round(beats * 60 / ctx.tempo * sr)), 1, capacity);
                wait = ctx.playing ? std::max(0, int(std::round((std::ceil((ctx.beat - 1e-9) / 4) * 4 - ctx.beat) * 60 /
                                                                ctx.tempo * sr)))
                                   : 0;
            }
            if (state == 3) {
                ++pass;
                if (!pass) {
                    pass = 1;
                    std::fill(backed.begin(), backed.end(), 0);
                }
                undoAvailable = false;
            }
        }
        if (auto* pending = requested.exchange(nullptr, std::memory_order_acq_rel)) {
            if (capture)
                pending->state.store(-1, std::memory_order_release);
            else if (length <= 0 || state == 1 || state == 3 || pending->stereo.size() < size_t(length) * 2)
                pending->state.store(-1, std::memory_order_release);
            else {
                capture = pending;
                captureAt = 0;
                captureRevision = revision;
                capture->frames = length;
                capture->rate = int(sr);
            }
        }
        if (capture) {
            if (captureRevision != revision || state == 1 || state == 3)
                invalidateCapture();
            else {
                int end = std::min(length, captureAt + 8192);
                for (int i = captureAt; i < end; ++i) {
                    capture->stereo[size_t(i) * 2] = current(ctx.sample, i, 0);
                    capture->stereo[size_t(i) * 2 + 1] = current(ctx.sample, i, 1);
                }
                captureAt = end;
                if (captureAt == length) {
                    capture->state.store(1, std::memory_order_release);
                    capture = nullptr;
                }
            }
        }
        const float gain = dbToGain(pv(t, p, Output)), mix = p[Mix], feedback = p[Feedback];
        for (int i = 0; i < n; ++i) {
            float in[2] = {l[i], r[i]}, loop[2] = {};
            if (state == 1) {
                if (wait > 0)
                    --wait;
                else {
                    data[size_t(recorded) * 2] = in[0];
                    data[size_t(recorded) * 2 + 1] = in[1];
                    written[recorded] = generation;
                    ++recorded;
                    if (recorded >= targetLength) {
                        length = recorded;
                        position = 0;
                        state = 2;
                        ++revision;
                    }
                }
            } else if ((state == 2 || state == 3) && length > 0) {
                int at = std::clamp(int(position), 0, length - 1), next = (at + 1) % length;
                float fraction = float(position - at);
                for (int c = 0; c < 2; ++c)
                    loop[c] = current(ctx.sample, at, c) * (1 - fraction) + current(ctx.sample, next, c) * fraction;
                if (state == 3 && !undoSelected) {
                    dirty = true;
                    if (backed[at] != pass) {
                        for (int c = 0; c < 2; ++c)
                            backup[size_t(at) * 2 + c] = current(ctx.sample, at, c);
                        backed[at] = pass;
                    }
                    for (int c = 0; c < 2; ++c)
                        data[size_t(at) * 2 + c] = std::clamp(loop[c] * feedback + in[c], -8.f, 8.f);
                    written[at] = generation;
                    undoAvailable = true;
                    ++revision;
                }
                double step = state == 3 ? 1 : (pb(p, HalfSpeed) ? .5 : 1) * (pb(p, Reverse) ? -1 : 1);
                position += step;
                if (position >= length)
                    position -= length;
                if (position < 0)
                    position += length;
            }
            float g = output.next(gain, onePoleCoef(.01f, sr));
            l[i] = (in[0] * (state == 1 || state == 3 ? 1 : 1 - mix) + loop[0] * mix) * g;
            r[i] = (in[1] * (state == 1 || state == 3 ? 1 : 1 - mix) + loop[1] * mix) * g;
        }
        scope[0].store(length ? float(position / length) : 0, std::memory_order_relaxed);
        scope[1].store(float(length), std::memory_order_relaxed);
        scope[2].store(float(state), std::memory_order_relaxed);
        scope[3].store(float(revision), std::memory_order_relaxed);
        scope[4].store(undoAvailable ? 1 : 0, std::memory_order_relaxed);
        scope[5].store(wait > 0 ? 1 : 0, std::memory_order_relaxed);
        scope[6].store(dirty ? 1 : 0, std::memory_order_relaxed);
        scope[7].store(undoSelected ? 1 : 0, std::memory_order_relaxed);
    }
    void panic() override {
        state = 0;
        lastRequested = 0;
        position = 0;
        invalidateCapture();
        output.primed = false;
    }
};
} // namespace
std::shared_ptr<DeviceProcessor> createLooper(float sr) {
    return std::make_shared<Looper>(sr);
}
} // namespace sessions::audio
