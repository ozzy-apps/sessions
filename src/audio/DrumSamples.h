#pragma once
#include <QString>
#include <array>
#include <vector>
#include <memory>

namespace sessions::audio::drums {
struct Layer { std::vector<float> mono, right; double rate = 44100; };
struct Bank { std::array<std::array<Layer, 5>, 128> notes; };
// Immutable, shared by all processors. Prepared on the UI thread when a processor is created.
const std::array<Bank, 3>& library();
const char* sourceName(int recipe);
int bankFor(int recipe);
int sampleNote(int gm);
float sampleTune(int gm);
QString error();
int loadedLayers();

struct RecordedHit {
    Layer close, alternate, overhead, room;
    float gain = 1;
};
struct VelocityGroup { int low = 1, high = 127; std::vector<RecordedHit> hits; };
struct ContentBank {
    QString name, error;
    std::array<std::vector<VelocityGroup>,128> notes;
    size_t frames = 0;
    int recordings = 0;
};
struct Content { std::array<std::shared_ptr<const ContentBank>,6> banks; };
// All preparation is called on the UI side. Content is immutable during rendering.
int contentIndex(const float* params, int drum);
const char* contentName(int index);
bool contentInstalled(int index);
std::shared_ptr<const ContentBank> loadContentBank(int index);
std::shared_ptr<const Content> prepareContent(const float* params, bool all = false);
const RecordedHit* recordedHit(const ContentBank& bank, int note, int velocity, unsigned roundRobin, int* layers = nullptr);
}
