#pragma once
#include <array>
#include <complex>
#include <vector>
namespace sessions::stems {
constexpr int kFft = 4096, kHop = 1024, kModelFrames = 512, kModelBins = 1024, kSources = 5;
using Complex = std::complex<float>;
void fft(std::vector<Complex>& data, bool inverse = false);
std::vector<float> hannWindow();
// Joint softmax followed by power-2 ratio masking equals softmax(2 * logits).
std::array<float, kSources> ratioMasks(const std::array<float, kSources>& logits);
} // namespace sessions::stems
