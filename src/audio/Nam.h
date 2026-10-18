#pragma once
// Neural Amp Modeler captures (.nam): the file format of Steven Atkinson's
// NeuralAmpModelerCore (JSON: "version", "architecture", "config", a flat "weights" array,
// optional "sample_rate" and "metadata"), read into a Model on the UI thread, and the networks
// run in plain C++ on the audio thread: WaveNet (the trainer's standard / lite / feather / nano
// shapes, gated or not, plus kernel-size lists, per-layer activations, bottlenecks, head 1x1s
// and a post-stack head), LSTM, Linear (an impulse response) and ConvNet. Weights are consumed
// in exactly the order NeuralAmpModelerCore reads them (its Conv1D / Conv1x1 / LSTMCell /
// BatchNorm set_weights_), checked against its example models.
//
// A Model is immutable and shared (nam::load caches it by path). A Runtime is one device's
// state for a model at the engine's rate: two channels of ring buffers (linked while the two
// sides carry the same signal, as a mono guitar input does), with windowed-sinc resamplers
// around the network when the engine doesn't run at the capture's rate. It's built (and
// prewarmed) on the UI thread in Engine::publish and only touched by the audio thread after
// that; nothing here allocates or locks on the audio thread.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

namespace sessions::nam {

constexpr int kMaxChannels = 128;   // channels of one layer; bigger models are refused
constexpr int kMaxFrames = 64;      // the network runs in chunks of this many frames
constexpr int kMaxLinearTaps = 16384;

enum class Arch { Linear, ConvNet, LSTM, WaveNet };

struct Activation {
    enum Type { Identity, Tanh, Hardtanh, ReLU, LeakyReLU, PReLU, Sigmoid, SiLU, Hardswish, Softsign, LeakyHardtanh };
    Type  type = Identity;
    float slope = 0.01f;                                               // LeakyReLU
    std::vector<float> slopes;                                         // PReLU: one per channel
    float minVal = -1, maxVal = 1, minSlope = 0.01f, maxSlope = 0.01f; // LeakyHardtanh
    void apply(float* x, int n) const;
};

// A causal convolution: `kernel` taps `dilation` samples apart (kernel 1: a 1x1 / fully
// connected layer). Weights are stored input-major, w[(tap * in + i) * out + o], so the inner
// loops run over the outputs with no reduction and vectorise.
struct Conv {
    int  in = 0, out = 0, kernel = 1, dilation = 1;
    bool bias = false;
    std::vector<float> w, b;
    int lookback() const { return (kernel - 1) * dilation; }
};

struct WaveLayer {
    int  channels = 0, bottleneck = 0;
    int  gating = 0;                 // 0 none, 1 gated: act(top) * act2(bottom), 2 blended
    Activation act, act2;
    Conv conv, mixin;                // the dilated conv on the layer's input, the condition's 1x1 added to it
    bool hasLayer1x1 = true, hasHead1x1 = false;
    Conv layer1x1, head1x1;          // towards the next layer (residual) and towards the head
};
struct WaveArray {
    int  inputSize = 0, channels = 0, bottleneck = 0, headIn = 0, headSize = 0;
    Conv rechannel;                  // input -> channels (no bias)
    std::vector<WaveLayer> layers;
    Conv headRechannel;              // the layers' summed head outputs -> headSize
};
struct WaveHead { std::vector<Conv> convs; Activation act; };   // the optional post-stack head
struct LstmLayer {
    int in = 0, hidden = 0;
    std::vector<float> w;            // [(in + hidden)][4 * hidden], input-major; gates in the order i, f, g, o
    std::vector<float> b, h0, c0;    // bias and the initial states (part of the weights)
};
struct ConvBlock { Conv conv; bool batchnorm = false; std::vector<float> scale, shift; Activation act; };

struct Model;
using ModelPtr = std::shared_ptr<const Model>;
// A lighter size of the same capture (a "SlimmableContainer" file holds several, trained
// together): the network to run when Quality is set to Light.
struct Variant { double maxValue = 0; ModelPtr model; };

struct Model {
    QString path, fileName;
    QString version, archName;       // the file's "version" and "architecture"
    Arch    arch = Arch::WaveNet;
    QString name;                    // metadata "name", else the file's base name
    QString summary;                 // "WaveNet · 16/8 channels · 2 × 10 layers"
    QString gear;                    // "Darkglass Electronics Microtubes 900 v2 (amp, clean)"
    QString modeledBy;
    bool    hasLoudness = false, hasGain = false, hasInputLevel = false, hasOutputLevel = false;
    double  loudness = 0, gain = 0, inputLevel = 0, outputLevel = 0;   // dB, dBu
    double  sampleRate = 48000;      // the rate it was trained at
    bool    rateKnown = false;       // the file said so (else 48 kHz is assumed, as the plugins do)
    int     weightCount = 0;
    int     receptiveField = 1;      // input samples that shape one output (the prewarm length)
    bool    container = false;       // a SlimmableContainer file: this is its full size, `variants` the lighter ones
    std::vector<Variant> variants;   // lighter sizes, ascending (empty for a single-size capture)
    int     channelsSummary = 0;     // the WaveNet's first-array channels (for the panel's size note)
    // WaveNet
    std::vector<WaveArray> arrays;
    bool     hasHead = false;
    WaveHead head;
    float    headScale = 1;
    // LSTM
    std::vector<LstmLayer> lstm;
    Conv lstmHead;                   // hidden -> 1, with bias
    // Linear
    std::vector<float> ir;           // ir[0] weighs the current sample
    float irBias = 0;
    // ConvNet
    std::vector<ConvBlock> blocks;
    Conv convHead;
};

// Parses a .nam file's contents. Null, with `error`, when it isn't one, uses an architecture
// or feature this reader doesn't have, or its weights don't fit the architecture.
ModelPtr parse(const QByteArray& json, QString* error = nullptr, const QString& path = {});
// Reads a .nam file (cached by path, modification time and size). Null while the file is missing or bad.
ModelPtr load(const QString& path, QString* error = nullptr);
bool        isModelFile(const QString& path);   // by extension
QStringList nameFilters();                      // "*.nam"
// Writes a small WaveNet capture with random weights (an 8 / 4 channel shape like the
// trainer's "feather"), for screenshots and tests. `seed` picks the weights.
bool writeExample(const QString& path, unsigned seed = 1);
// The fast hyperbolic tangent the networks use (a rational approximation good to ~1e-7).
float fastTanh(float x);

// One channel of audio through the network: the ring buffers behind every convolution and
// the LSTM states. Allocated once for a model; process() never allocates.
class Channel {
public:
    explicit Channel(const Model& m);
    void process(const float* in, float* out, int n);   // any n (chunks of kMaxFrames inside)
    void clear();                                        // zero history (an unwarmed network)
    void copyStateFrom(const Channel& o);                // same model
private:
    // A convolution's input history: per channel a ring of `size` frames written twice (mirrored),
    // so any run of up to `size` frames back from the newest is one contiguous stretch.
    struct Hist {
        std::vector<float> data;   // channels x (2 * size)
        int channels = 0, size = 0, mask = 0, pos = 0;   // pos: where the next frame goes
        void init(int ch, int minFrames);
        void write(const float* block, int n);           // n frames, channel-major (block[c * n + t])
        void push(const float* frame);                   // one frame
        const float* run(int c, int lag, int from) const { return &data[size_t(c) * 2 * size_t(size) + size_t((from - lag) & mask)]; }
        float at(int c, int lag) const { return data[size_t(c) * 2 * size_t(size) + size_t((pos - 1 - lag) & mask)]; }
    };
    void chunk(const float* in, float* out, int n);
    void waveNet(const float* in, float* out, int n);
    void lstm(const float* in, float* out, int n);
    void linear(const float* in, float* out, int n);
    void convNet(const float* in, float* out, int n);
    const Model& m_m;
    std::vector<Hist>  m_hist;       // every convolution's input history, in network order
    std::vector<float> m_h, m_c;     // LSTM states, layer after layer
    std::vector<float> m_a, m_b, m_z, m_acc, m_headOut, m_pre, m_cond;   // channel-major chunk scratch
    std::vector<float> m_irRev;      // Linear: the impulse response reversed
};

// A device's runtime for a model at the engine's rate.
class Runtime {
public:
    Runtime(ModelPtr model, float engineRate);
    ~Runtime();
    const ModelPtr& model() const { return m_model; }
    float engineRate() const { return m_engineRate; }
    bool  resampling() const { return m_resampling; }
    int   latency() const { return m_latency; }   // engine frames the resampling delays the sound (0 without it)
    void  process(float* L, float* R, int n);     // in place; audio thread; n <= audio::kMaxBlock
    void  reset();                                // audio thread: back to the prewarmed state
private:
    class Resampler;
    struct Side;
    void run(Channel& ch, Side* s, float* x, int n);
    ModelPtr m_model;
    float    m_engineRate;
    bool     m_resampling = false, m_linked = true;
    int      m_latency = 0;
    std::unique_ptr<Channel> m_left, m_right, m_warm;
    std::unique_ptr<Side> m_sideL, m_sideR;
};

} // namespace sessions::nam
