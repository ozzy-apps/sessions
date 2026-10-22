#pragma once
#include <QObject>
#include <QString>
#include <array>
#include <vector>
#include <memory>
#include <tuple>

namespace sessions::audio::drums {
// A recording. Long recorded-library hits are streamed (DrumStream.h): `mono` / `right` then hold
// only the head, `stream` names the file the rest comes from and `frames` is the whole length.
struct Layer {
    std::vector<float> mono, right; double rate = 44100;
    int    stream = -1;
    qint64 frames = 0;
    qint64 length() const { return frames > 0 ? frames : qint64(mono.size()); }
};
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
    size_t frames = 0;      // samples held in memory (heads, for streamed recordings)
    int recordings = 0;
    int streamed = 0;       // recordings whose tails stream from disk
    qint64 wholeFrames = 0; // samples if every recording were held whole
};
constexpr int kContentBanks = 6, kContentDrums = 8;   // the six libraries; Drum Studio's eight drums
struct Content {
    std::array<std::shared_ptr<const ContentBank>,kContentBanks> banks;
    // While a drum's library is still loading it plays the one it played before (its bank is kept
    // in `banks`), else the built-in kit: -1. `effective` is what each drum played at publish time.
    std::array<int,kContentDrums> fallback{-1,-1,-1,-1,-1,-1,-1,-1}, effective{-1,-1,-1,-1,-1,-1,-1,-1};
};
// All preparation is called on the UI side. Content is immutable during rendering.
int contentIndex(const float* params, int drum);
const char* contentName(int index);
bool contentInstalled(int index);
// Decodes a bank on the calling thread (offline renders, tests) and shares it with the loader;
// a bank the loader is decoding is waited for.
std::shared_ptr<const ContentBank> loadContentBank(int index);
std::shared_ptr<const Content> prepareContent(const float* params, bool all = false);
const RecordedHit* recordedHit(const ContentBank& bank, int note, int velocity, unsigned roundRobin, int* layers = nullptr);
// The banks a Drum Studio can reach: its library and per-drum sources as set, and every choice
// automation, clip envelopes and macros move them through. `ranges`: (param, low, high) positions.
std::array<bool,kContentBanks> reachableBanks(const float* params, const std::vector<std::tuple<int,float,float>>& ranges);

// Loads libraries in the background for the live engine, so choosing a kit never stalls the
// window. One worker decodes one bank at a time; a bank nobody wants any more is cancelled.
// Banks in use stay loaded; others are kept, most recently used first, while all decoded banks
// together stay within the memory budget, so switching back is instant. UI thread only.
class ContentLoader : public QObject {
    Q_OBJECT
public:
    static ContentLoader* instance();
    enum class State { Missing, Idle, Queued, Loading, Ready, Failed };
    struct Status { State state = State::Idle; double progress = 0; QString error; qint64 bytes = 0; };
    Status status(int bank) const;
    std::shared_ptr<const ContentBank> ready(int bank) const;   // decoded, else nullptr; never waits
    void   want(const std::array<bool,kContentBanks>& banks);   // the live engine's banks
    bool   wanted(int bank) const;
    qint64 memoryUsed() const;                                    // decoded banks held, in bytes
    qint64 memoryWhole() const;                                   // what they would hold without streaming
    qint64 budget() const { return m_budget; }
    void   setBudget(qint64 bytes);                               // 0: no limit
    static qint64 savedBudget();                                  // Preferences ("drums/memoryBudgetGB"; 4 GB to begin with)
    void   reload();   // read the libraries again (Preferences: streaming changed); each plays as before until it's ready
    void   setReadDelayForTests(int ms) { m_testDelay = ms; }     // slows each recording's read
    bool   waitIdle(int ms);                                      // tests: until nothing loads (false on timeout)
signals:
    void progressed(int bank);   // a load moved on (Drum Studio's status line)
    void finished(int bank);     // a load ended: ready, failed or cancelled (the engine republishes)
private:
    ContentLoader();
    void start();                // the next wanted bank, when the worker is free
    void trim();                 // keep unused banks within the budget
    void done(int bank, std::shared_ptr<const ContentBank> result, bool cancelled);
    friend std::shared_ptr<const ContentBank> loadContentBank(int index);
    std::array<bool,kContentBanks> m_wanted{};
    std::array<qint64,kContentBanks> m_lastWanted{};
    qint64 m_budget = 0;
    int    m_testDelay = 0;
    int    m_running = -1;
};
}
