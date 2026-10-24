#pragma once
#include "Dsp.h"
#include <array>

namespace sessions::audio {
template <int N> class Fourier {
    std::array<int, N> rev{};
    std::array<std::complex<float>, N / 2> tw{};

  public:
    Fourier() {
        int bits = 0;
        for (int n = N; n > 1; n >>= 1)
            ++bits;
        for (int i = 0; i < N; ++i) {
            int x = i, v = 0;
            for (int b = 0; b < bits; ++b) {
                v = (v << 1) | (x & 1);
                x >>= 1;
            }
            rev[i] = v;
        }
        for (int i = 0; i < N / 2; ++i)
            tw[i] = std::polar(1.f, -dsp::kTwoPi * i / N);
    }
    void run(std::complex<float>* a, bool inverse = false) const {
        for (int i = 0; i < N; ++i)
            if (rev[i] > i)
                std::swap(a[i], a[rev[i]]);
        for (int len = 2; len <= N; len *= 2)
            for (int base = 0; base < N; base += len)
                for (int j = 0; j < len / 2; ++j) {
                    auto w = tw[j * N / len];
                    if (inverse)
                        w = std::conj(w);
                    auto u = a[base + j], v = a[base + j + len / 2] * w;
                    a[base + j] = u + v;
                    a[base + j + len / 2] = u - v;
                }
        if (inverse)
            for (int i = 0; i < N; ++i)
                a[i] /= N;
    }
};
} // namespace sessions::audio
