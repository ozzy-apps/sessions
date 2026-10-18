#pragma once
// Single-producer / single-consumer lock-free ring buffer. One thread pushes,
// one other thread pops; neither ever blocks or allocates.

#include <QtGlobal>
#include <atomic>

namespace sessions::audio {

template <class T, int N>
class SpscQueue {
public:
    bool push(const T& v) {
        int h = m_head.load(std::memory_order_relaxed);
        int next = (h + 1) % N;
        if (next == m_tail.load(std::memory_order_acquire)) return false;   // full
        m_buf[h] = v;
        m_head.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& v) {
        int t = m_tail.load(std::memory_order_relaxed);
        if (t == m_head.load(std::memory_order_acquire)) return false;      // empty
        v = m_buf[t];
        m_tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }
    bool full() const {
        return (m_head.load(std::memory_order_acquire) + 1) % N == m_tail.load(std::memory_order_acquire);
    }

private:
    T m_buf[N] {};
    std::atomic<int> m_head{0}, m_tail{0};
};

} // namespace sessions::audio

namespace sessions::audio {

// A lock-free ring of floats for audio streams (input capture, recording): one thread
// writes blocks, another reads them. The capacity is fixed at construction (a power of
// two); writes that don't fit are cut short and reported, nothing ever blocks.
class SpscFloatRing {
public:
    explicit SpscFloatRing(int capacityPow2 = 1 << 18) : m_buf(new float[size_t(capacityPow2)]()), m_cap(capacityPow2), m_mask(capacityPow2 - 1) {}
    ~SpscFloatRing() { delete[] m_buf; }
    SpscFloatRing(const SpscFloatRing&) = delete;
    SpscFloatRing& operator=(const SpscFloatRing&) = delete;

    int capacity() const { return m_cap; }
    int available() const {   // floats ready to read
        return int(m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_acquire));
    }
    int space() const { return m_cap - available(); }
    // Writer: copies up to n floats, returns how many fit.
    int write(const float* src, int n) {
        const quint64 h = m_head.load(std::memory_order_relaxed);
        const int room = m_cap - int(h - m_tail.load(std::memory_order_acquire));
        n = n < room ? n : room;
        for (int i = 0; i < n; ++i) m_buf[(h + quint64(i)) & quint64(m_mask)] = src[i];
        m_head.store(h + quint64(n), std::memory_order_release);
        return n;
    }
    // Reader: copies up to n floats (dst may be null to skip them), returns how many it got.
    int read(float* dst, int n) {
        const quint64 t = m_tail.load(std::memory_order_relaxed);
        const int have = int(m_head.load(std::memory_order_acquire) - t);
        n = n < have ? n : have;
        if (dst) for (int i = 0; i < n; ++i) dst[i] = m_buf[(t + quint64(i)) & quint64(m_mask)];
        m_tail.store(t + quint64(n), std::memory_order_release);
        return n;
    }

private:
    float* m_buf;
    int m_cap, m_mask;
    std::atomic<quint64> m_head{0}, m_tail{0};
};

} // namespace sessions::audio
