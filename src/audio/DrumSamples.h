#pragma once
#include <QString>
#include <array>
#include <vector>

namespace sessions::audio::drums {
struct Layer { std::vector<float> mono; double rate = 44100; };
struct Bank { std::array<std::array<Layer, 5>, 128> notes; };
// Immutable, shared by all processors. Prepared on the UI thread when a processor is created.
const std::array<Bank, 3>& library();
const char* sourceName(int recipe);
int bankFor(int recipe);
int sampleNote(int gm);
float sampleTune(int gm);
QString error();
int loadedLayers();
}
