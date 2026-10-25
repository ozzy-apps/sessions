// Neural Amp Modeler captures: the .nam reader, the networks and a device's runtime. See Nam.h.
//
// The weight order below is NeuralAmpModelerCore's, module by module:
//   Conv1D / Conv1x1:  for each group, for each output channel, for each input channel (of the
//                      group), for each kernel tap: W[tap](out, in); then the biases, one per
//                      output channel. Tap k reads the input (kernel - 1 - k) * dilation samples
//                      back, so the last tap is the current sample.
//   WaveNet:           per layer array: the rechannel 1x1 (no bias); per layer: the dilated conv
//                      (bias), the condition's 1x1 ("input mixin", no bias), the layer 1x1 towards
//                      the next layer (bias) when active, the head 1x1 (bias) when active; then the
//                      head rechannel (a conv, kernel 1 in the classic files). After every array
//                      the optional post-stack head's convs, then one float: head_scale.
//   LSTM:              per layer: W (4·hidden rows by input + hidden columns, row-major, gates
//                      i f g o), the bias (4·hidden), the initial hidden state, the initial cell
//                      state; then the head's weights (hidden) and its bias.
//   Linear:            the impulse response (ir[0] on the current sample), then the bias.
//   ConvNet:           per block: the kernel-2 conv (bias only without batchnorm), then batchnorm's
//                      running mean, running variance, weight, bias (one each per channel) and
//                      eps; then the head: weights (channels) and its bias.

#include "Nam.h"
#include "Convolver.h"
#include "DeviceKit.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <cstring>

// The hot loops are compiled twice on x86-64 (AVX2 + FMA, and the baseline), picked at load time
// by the CPU: a generic build still runs a standard capture at about a tenth of a core.
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(__SANITIZE_ADDRESS__) && !defined(_WIN32)
#define NAM_HOT __attribute__((target_clones("avx2,fma", "default")))
#else
#define NAM_HOT
#endif

namespace sessions::nam {

// ============================================================================ math

// A rational approximation of tanh (the one Eigen and TensorFlow use for floats): a 13th-order
// odd polynomial over a 6th-order even one, clamped where tanh is 1 to float precision.
float fastTanh(float x) {
    x = std::clamp(x, -7.99881172180175781f, 7.99881172180175781f);
    const float x2 = x * x;
    float p = x2 * -2.76076847742355e-16f + 2.00018790482477e-13f;
    p = p * x2 + -8.60467152213735e-11f;
    p = p * x2 + 5.12229709037114e-08f;
    p = p * x2 + 1.48572235717979e-05f;
    p = p * x2 + 6.37261928875436e-04f;
    p = p * x2 + 4.89352455891786e-03f;
    p = p * x;
    float q = x2 * 1.19825839466702e-06f + 1.18534705686654e-04f;
    q = q * x2 + 2.26843463243900e-03f;
    q = q * x2 + 4.89352518554385e-03f;
    return p / q;
}

namespace {

inline float sigmoid(float x) { return 0.5f * (1.0f + fastTanh(0.5f * x)); }

} // namespace

NAM_HOT void Activation::apply(float* x, int n) const {
    switch (type) {
    case Identity: return;
    case Tanh:      for (int i = 0; i < n; ++i) x[i] = fastTanh(x[i]); return;
    case Hardtanh:  for (int i = 0; i < n; ++i) x[i] = std::clamp(x[i], -1.0f, 1.0f); return;
    case ReLU:      for (int i = 0; i < n; ++i) x[i] = x[i] > 0 ? x[i] : 0.0f; return;
    case LeakyReLU: for (int i = 0; i < n; ++i) x[i] = x[i] > 0 ? x[i] : slope * x[i]; return;
    case PReLU:
        for (int i = 0; i < n; ++i) {
            const float s = slopes.empty() ? slope : slopes[size_t(i) % slopes.size()];
            x[i] = x[i] > 0 ? x[i] : s * x[i];
        }
        return;
    case Sigmoid:   for (int i = 0; i < n; ++i) x[i] = sigmoid(x[i]); return;
    case SiLU:      for (int i = 0; i < n; ++i) x[i] = x[i] * sigmoid(x[i]); return;
    case Hardswish: for (int i = 0; i < n; ++i) x[i] = x[i] * std::clamp(x[i] + 3.0f, 0.0f, 6.0f) * (1.0f / 6.0f); return;
    case Softsign:  for (int i = 0; i < n; ++i) x[i] = x[i] / (1.0f + std::fabs(x[i])); return;
    case LeakyHardtanh:
        for (int i = 0; i < n; ++i) {
            const float v = x[i];
            x[i] = v < minVal ? (v - minVal) * minSlope + minVal : v > maxVal ? (v - maxVal) * maxSlope + maxVal : v;
        }
        return;
    }
}

namespace {

constexpr int kMaxKernel = 32;   // taps of one convolution (the A2 captures use 15)

// ============================================================================ reading the file

// Walks the flat weight array. Without a vector it only counts (writeExample sizes a file that way).
struct Cursor {
    const std::vector<float>* w = nullptr;
    size_t at = 0;
    float next() { const float v = w && at < w->size() ? (*w)[at] : 0.0f; ++at; return v; }
};

struct Reader {
    Cursor cur;
    QString error;
    bool fail(const QString& why) { if (error.isEmpty()) error = why; return false; }
};

int    jint(const QJsonObject& o, const char* key, int def) { const QJsonValue v = o.value(QLatin1String(key)); return v.isDouble() ? v.toInt() : def; }
bool   jbool(const QJsonObject& o, const char* key, bool def) { const QJsonValue v = o.value(QLatin1String(key)); return v.isBool() ? v.toBool() : def; }
double jnum(const QJsonObject& o, const char* key, double def) { const QJsonValue v = o.value(QLatin1String(key)); return v.isDouble() ? v.toDouble() : def; }
bool   present(const QJsonObject& o, const char* key) { const QJsonValue v = o.value(QLatin1String(key)); return !v.isUndefined() && !v.isNull(); }

// A conv's weights in NeuralAmpModelerCore's order into the input-major layout (zeros off the
// block diagonal when grouped).
bool readConv(Reader& r, Conv& c, int in, int out, int kernel, bool bias, int dilation, int groups = 1) {
    if (in <= 0 || out <= 0 || kernel <= 0 || dilation <= 0 || groups <= 0) return r.fail("A layer has a size of zero.");
    if (in > kMaxChannels || out > kMaxChannels) return r.fail(QString("A layer has %1 channels; up to %2 are supported.").arg(std::max(in, out)).arg(kMaxChannels));
    if (kernel > kMaxKernel) return r.fail(QString("A convolution has %1 taps; up to %2 are supported.").arg(kernel).arg(kMaxKernel));
    if (in % groups || out % groups) return r.fail("A layer's channels don't divide into its groups.");
    c.in = in; c.out = out; c.kernel = kernel; c.bias = bias; c.dilation = dilation;
    c.w.assign(size_t(kernel) * in * out, 0.0f);
    c.b.assign(bias ? size_t(out) : 0, 0.0f);
    const int opg = out / groups, ipg = in / groups;
    for (int g = 0; g < groups; ++g)
        for (int i = 0; i < opg; ++i)
            for (int j = 0; j < ipg; ++j)
                for (int k = 0; k < kernel; ++k)
                    c.w[(size_t(k) * in + size_t(g * ipg + j)) * out + size_t(g * opg + i)] = r.cur.next();
    if (bias) for (int o = 0; o < out; ++o) c.b[size_t(o)] = r.cur.next();
    return true;
}

bool parseActivation(Reader& r, const QJsonValue& v, Activation& a) {
    QString name;
    QJsonObject o;
    if (v.isString()) name = v.toString();
    else if (v.isObject()) { o = v.toObject(); name = o.value("type").toString(); }
    else return r.fail("An activation isn't a name or an object.");
    static const struct { const char* name; Activation::Type type; } kTypes[] = {
        {"Tanh", Activation::Tanh}, {"Fasttanh", Activation::Tanh}, {"Hardtanh", Activation::Hardtanh}, {"ReLU", Activation::ReLU},
        {"LeakyReLU", Activation::LeakyReLU}, {"PReLU", Activation::PReLU}, {"Sigmoid", Activation::Sigmoid}, {"SiLU", Activation::SiLU},
        {"Hardswish", Activation::Hardswish}, {"Softsign", Activation::Softsign}, {"LeakyHardtanh", Activation::LeakyHardtanh}};
    bool found = false;
    for (const auto& t : kTypes) if (name == QLatin1String(t.name)) { a.type = t.type; found = true; }
    if (!found) return r.fail(QString("The activation \"%1\" isn't supported.").arg(name));
    a.slope = float(jnum(o, "negative_slope", 0.01));
    a.slopes.clear();
    for (const QJsonValue& s : o.value("negative_slopes").toArray()) a.slopes.push_back(float(s.toDouble()));
    a.minVal = float(jnum(o, "min_val", -1)); a.maxVal = float(jnum(o, "max_val", 1));
    a.minSlope = float(jnum(o, "min_slope", 0.01)); a.maxSlope = float(jnum(o, "max_slope", 0.01));
    return true;
}

// One activation for every layer, or a list with one per layer. With `allowNull`, a null entry
// (what the trainer writes as the secondary activation of a layer that isn't gated) is Identity.
bool parseActivations(Reader& r, const QJsonValue& v, int layers, std::vector<Activation>& out, bool allowNull = false) {
    out.assign(size_t(layers), Activation{});
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() != layers) return r.fail("A layer array's activation list doesn't match its dilations.");
        for (int i = 0; i < layers; ++i) {
            if (allowNull && (a[i].isNull() || a[i].isUndefined())) continue;
            if (!parseActivation(r, a[i], out[size_t(i)])) return false;
        }
        return true;
    }
    if (allowNull && (v.isNull() || v.isUndefined())) return true;
    Activation one;
    if (!parseActivation(r, v, one)) return false;
    std::fill(out.begin(), out.end(), one);
    return true;
}

bool parseWaveNet(Reader& r, const QJsonObject& config, Model& m) {
    if (jint(config, "in_channels", 1) != 1) return r.fail("Only mono (one input channel) captures are supported.");
    if (present(config, "condition_dsp")) return r.fail("Conditioned WaveNets (a condition DSP) aren't supported yet.");
    const QJsonArray layerArrays = config.value("layers").toArray();
    if (layerArrays.isEmpty()) return r.fail("The WaveNet has no layer arrays.");
    static const char* kFilms[] = {"conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film",
                                   "activation_pre_film", "activation_post_film", "layer1x1_post_film", "head1x1_post_film"};
    for (const QJsonValue& lv : layerArrays) {
        const QJsonObject lc = lv.toObject();
        WaveArray A;
        A.channels = jint(lc, "channels", 0);
        A.bottleneck = jint(lc, "bottleneck", A.channels);
        A.inputSize = jint(lc, "input_size", 0);
        const int condition = jint(lc, "condition_size", 1);
        if (condition != 1) return r.fail("Only a one-channel condition (the input itself) is supported.");
        const int groups = jint(lc, "groups_input", 1), groupsMixin = jint(lc, "groups_input_mixin", 1);
        bool layer1x1 = true;
        int layer1x1Groups = 1;
        if (present(lc, "layer1x1")) { const QJsonObject l = lc.value("layer1x1").toObject(); layer1x1 = jbool(l, "active", true); layer1x1Groups = jint(l, "groups", 1); }
        bool head1x1 = false;
        int head1x1Out = A.channels, head1x1Groups = 1;
        if (present(lc, "head1x1")) { const QJsonObject h = lc.value("head1x1").toObject(); head1x1 = jbool(h, "active", false); head1x1Out = jint(h, "out_channels", A.channels); head1x1Groups = jint(h, "groups", 1); }
        for (const char* film : kFilms) {
            const QJsonValue f = lc.value(QLatin1String(film));
            if (f.isObject() && jbool(f.toObject(), "active", true)) return r.fail("FiLM conditioning (a newer WaveNet feature) isn't supported yet.");
        }
        int headKernel = 1, headDilation = 1;
        bool headBias = false;
        if (present(lc, "head")) {
            const QJsonObject h = lc.value("head").toObject();
            A.headSize = jint(h, "out_channels", 0);
            headKernel = jint(h, "kernel_size", 1);
            headDilation = jint(h, "head_dilation", 1);
            headBias = jbool(h, "bias", false);
        } else {
            A.headSize = jint(lc, "head_size", 0);
            headBias = jbool(lc, "head_bias", false);
        }
        const QJsonArray dilations = lc.value("dilations").toArray();
        const int layers = dilations.size();
        if (layers == 0) return r.fail("A layer array has no dilations.");
        std::vector<int> kernels(size_t(layers), jint(lc, "kernel_size", 0));
        if (present(lc, "kernel_sizes")) {
            const QJsonArray ks = lc.value("kernel_sizes").toArray();
            if (ks.size() != layers) return r.fail("A layer array's kernel sizes don't match its dilations.");
            for (int i = 0; i < layers; ++i) kernels[size_t(i)] = ks[i].toInt();
        }
        std::vector<Activation> acts;
        if (!parseActivations(r, lc.value("activation"), layers, acts)) return false;
        // gating: the new "gating_mode" (with a secondary activation, Sigmoid unless said), or the old "gated" flag
        std::vector<int> gating(size_t(layers), 0);
        std::vector<Activation> acts2(size_t(layers), Activation{});
        auto mode = [&](const QString& s, int& g) {
            if (s == "gated") g = 1; else if (s == "blended") g = 2; else if (s == "none") g = 0;
            else return r.fail(QString("The gating mode \"%1\" isn't known.").arg(s));
            return true;
        };
        if (present(lc, "gating_mode")) {
            const QJsonValue gm = lc.value("gating_mode");
            if (gm.isArray()) {
                const QJsonArray a = gm.toArray();
                if (a.size() != layers) return r.fail("A layer array's gating modes don't match its dilations.");
                for (int i = 0; i < layers; ++i) if (!mode(a[i].toString(), gating[size_t(i)])) return false;
            } else {
                int g = 0;
                if (!mode(gm.toString(), g)) return false;
                std::fill(gating.begin(), gating.end(), g);
            }
            if (present(lc, "secondary_activation")) {
                if (!parseActivations(r, lc.value("secondary_activation"), layers, acts2, true)) return false;
            }
            for (int l = 0; l < layers; ++l)   // a gated layer without one gets Sigmoid, as the core does
                if (gating[size_t(l)] && acts2[size_t(l)].type == Activation::Identity) acts2[size_t(l)].type = Activation::Sigmoid;
        } else if (jbool(lc, "gated", false)) {
            Activation sig; sig.type = Activation::Sigmoid;
            std::fill(gating.begin(), gating.end(), 1);
            std::fill(acts2.begin(), acts2.end(), sig);
        }
        if (!layer1x1 && A.bottleneck != A.channels) return r.fail("A layer array without layer 1x1s needs its bottleneck equal to its channels.");
        if (!readConv(r, A.rechannel, A.inputSize, A.channels, 1, false, 1)) return false;
        for (int l = 0; l < layers; ++l) {
            WaveLayer L;
            L.channels = A.channels;
            L.bottleneck = A.bottleneck;
            L.gating = gating[size_t(l)];
            L.act = acts[size_t(l)];
            L.act2 = acts2[size_t(l)];
            const int zw = (L.gating ? 2 : 1) * A.bottleneck;
            if (zw > kMaxChannels) return r.fail(QString("A gated layer needs %1 channels; up to %2 are supported.").arg(zw).arg(kMaxChannels));
            if (!readConv(r, L.conv, A.channels, zw, kernels[size_t(l)], true, dilations[l].toInt(), groups)) return false;
            if (!readConv(r, L.mixin, condition, zw, 1, false, 1, groupsMixin)) return false;
            L.hasLayer1x1 = layer1x1;
            if (layer1x1 && !readConv(r, L.layer1x1, A.bottleneck, A.channels, 1, true, 1, layer1x1Groups)) return false;
            L.hasHead1x1 = head1x1;
            if (head1x1 && !readConv(r, L.head1x1, A.bottleneck, head1x1Out, 1, true, 1, head1x1Groups)) return false;
            A.layers.push_back(std::move(L));
        }
        A.headIn = head1x1 ? head1x1Out : A.bottleneck;
        if (!readConv(r, A.headRechannel, A.headIn, A.headSize, headKernel, headBias, headDilation)) return false;
        if (!m.arrays.empty()) {
            const WaveArray& prev = m.arrays.back();
            if (A.inputSize != prev.channels) return r.fail("A layer array's input size doesn't match the channels before it.");
            if (A.headIn != prev.headSize) return r.fail("A layer array's head input doesn't match the head size before it.");
        }
        m.arrays.push_back(std::move(A));
    }
    m.hasHead = present(config, "head");
    if (m.hasHead) {
        const QJsonObject h = config.value("head").toObject();
        const int channels = jint(h, "channels", 0), out = jint(h, "out_channels", 1);
        if (out != 1) return r.fail("Only mono (one output channel) captures are supported.");
        const QJsonArray ks = h.value("kernel_sizes").toArray();
        if (ks.isEmpty()) return r.fail("The post-stack head has no kernel sizes.");
        if (!parseActivation(r, h.value("activation"), m.head.act)) return false;
        int cin = m.arrays.back().headSize;
        for (int i = 0; i < ks.size(); ++i) {
            Conv c;
            if (!readConv(r, c, cin, i + 1 == ks.size() ? out : channels, ks[i].toInt(), true, 1)) return false;
            cin = c.out;
            m.head.convs.push_back(std::move(c));
        }
    } else if (m.arrays.back().headSize != 1) {
        return r.fail("Only mono (one output channel) captures are supported.");
    }
    m.headScale = r.cur.next();
    int rf = 1;
    for (const WaveArray& A : m.arrays) {
        for (const WaveLayer& L : A.layers) rf += L.conv.lookback();
        rf += A.headRechannel.lookback();
    }
    for (const Conv& c : m.head.convs) rf += c.lookback();
    m.receptiveField = rf;
    return true;
}

bool parseLSTM(Reader& r, const QJsonObject& config, Model& m) {
    const int layers = jint(config, "num_layers", 0), input = jint(config, "input_size", 1), hidden = jint(config, "hidden_size", 0);
    if (jint(config, "in_channels", 1) != 1 || jint(config, "out_channels", 1) != 1 || input != 1) return r.fail("Only mono LSTM captures are supported.");
    if (layers <= 0 || hidden <= 0) return r.fail("The LSTM has no layers or no units.");
    if (4 * hidden > kMaxChannels * 2) return r.fail(QString("The LSTM has %1 units; up to %2 are supported.").arg(hidden).arg(kMaxChannels / 2));
    for (int l = 0; l < layers; ++l) {
        LstmLayer L;
        L.in = l == 0 ? input : hidden;
        L.hidden = hidden;
        const int cols = L.in + hidden, rows = 4 * hidden;
        L.w.assign(size_t(cols) * rows, 0.0f);
        for (int i = 0; i < rows; ++i)
            for (int j = 0; j < cols; ++j) L.w[size_t(j) * rows + size_t(i)] = r.cur.next();
        L.b.resize(size_t(rows)); for (float& x : L.b) x = r.cur.next();
        L.h0.resize(size_t(hidden)); for (float& x : L.h0) x = r.cur.next();
        L.c0.resize(size_t(hidden)); for (float& x : L.c0) x = r.cur.next();
        m.lstm.push_back(std::move(L));
    }
    if (!readConv(r, m.lstmHead, hidden, 1, 1, true, 1)) return false;
    m.receptiveField = int(m.sampleRate * 0.5);   // what the plugins warm an LSTM with
    return true;
}

bool parseLinear(Reader& r, const QJsonObject& config, Model& m) {
    const int rf = jint(config, "receptive_field", 0);
    if (jint(config, "in_channels", 1) != 1 || jint(config, "out_channels", 1) != 1) return r.fail("Only mono Linear captures are supported.");
    if (rf <= 0 || rf > kMaxLinearTaps) return r.fail(QString("A Linear capture of %1 taps; up to %2 are supported.").arg(rf).arg(kMaxLinearTaps));
    m.ir.resize(size_t(rf));
    for (float& x : m.ir) x = r.cur.next();
    m.irBias = jbool(config, "bias", false) ? r.cur.next() : 0.0f;
    m.receptiveField = rf;
    return true;
}

bool parseConvNet(Reader& r, const QJsonObject& config, Model& m) {
    const int channels = jint(config, "channels", 0), groups = jint(config, "groups", 1);
    const bool batchnorm = jbool(config, "batchnorm", false);
    if (jint(config, "in_channels", 1) != 1 || jint(config, "out_channels", 1) != 1) return r.fail("Only mono ConvNet captures are supported.");
    const QJsonArray dilations = config.value("dilations").toArray();
    if (channels <= 0 || dilations.isEmpty()) return r.fail("The ConvNet has no channels or no blocks.");
    Activation act;
    if (!parseActivation(r, config.value("activation"), act)) return false;
    int rf = 1;
    for (int i = 0; i < dilations.size(); ++i) {
        ConvBlock b;
        if (!readConv(r, b.conv, i == 0 ? 1 : channels, channels, 2, !batchnorm, dilations[i].toInt(), groups)) return false;
        b.batchnorm = batchnorm;
        if (batchnorm) {
            const size_t ch = size_t(channels);
            std::vector<float> mean(ch), var(ch), weight(ch), bias(ch);
            for (float& x : mean) x = r.cur.next();
            for (float& x : var) x = r.cur.next();
            for (float& x : weight) x = r.cur.next();
            for (float& x : bias) x = r.cur.next();
            const float eps = r.cur.next();
            b.scale.resize(size_t(channels)); b.shift.resize(size_t(channels));
            for (size_t c = 0; c < size_t(channels); ++c) {
                b.scale[c] = weight[c] / std::sqrt(eps + var[c]);
                b.shift[c] = bias[c] - b.scale[c] * mean[c];
            }
        }
        b.act = act;
        rf += b.conv.lookback();
        m.blocks.push_back(std::move(b));
    }
    if (!readConv(r, m.convHead, channels, 1, 1, true, 1)) return false;
    m.receptiveField = rf;
    return true;
}

// Reads a model's structure (and weights, when `weights` is given) from the file's root object.
bool build(Reader& r, const QJsonObject& root, const std::vector<float>* weights, Model& m) {
    r.cur = {weights, 0};
    m.version = root.value("version").toString();
    m.archName = root.value("architecture").toString();
    const QJsonObject config = root.value("config").toObject();
    if (present(root, "sample_rate")) { m.sampleRate = root.value("sample_rate").toDouble(); m.rateKnown = m.sampleRate > 0; }
    if (!m.rateKnown) m.sampleRate = 48000;
    const QJsonObject meta = root.value("metadata").toObject();
    m.name = meta.value("name").toString().trimmed();
    if (m.name.isEmpty()) m.name = m.fileName.isEmpty() ? QStringLiteral("Capture") : QFileInfo(m.fileName).completeBaseName();   // a container's submodels have no name of their own
    if (meta.value("loudness").isDouble()) { m.hasLoudness = true; m.loudness = meta.value("loudness").toDouble(); }
    if (meta.value("gain").isDouble()) { m.hasGain = true; m.gain = meta.value("gain").toDouble(); }
    if (meta.value("input_level_dbu").isDouble()) { m.hasInputLevel = true; m.inputLevel = meta.value("input_level_dbu").toDouble(); }
    if (meta.value("output_level_dbu").isDouble()) { m.hasOutputLevel = true; m.outputLevel = meta.value("output_level_dbu").toDouble(); }
    m.modeledBy = meta.value("modeled_by").toString().trimmed();
    QStringList gear;
    for (const char* k : {"gear_make", "gear_model"}) if (const QString s = meta.value(QLatin1String(k)).toString().trimmed(); !s.isEmpty()) gear << s;
    QStringList kind;
    for (const char* k : {"gear_type", "tone_type"}) if (const QString s = meta.value(QLatin1String(k)).toString().trimmed(); !s.isEmpty()) kind << s;
    m.gear = gear.join(' ') + (kind.isEmpty() ? QString() : (gear.isEmpty() ? QString() : QStringLiteral(" ")) + "(" + kind.join(", ") + ")");
    bool ok = false;
    if (m.archName == "WaveNet")      { m.arch = Arch::WaveNet; ok = parseWaveNet(r, config, m); }
    else if (m.archName == "LSTM")    { m.arch = Arch::LSTM;    ok = parseLSTM(r, config, m); }
    else if (m.archName == "Linear")  { m.arch = Arch::Linear;  ok = parseLinear(r, config, m); }
    else if (m.archName == "ConvNet") { m.arch = Arch::ConvNet; ok = parseConvNet(r, config, m); }
    else return r.fail(m.archName.isEmpty() ? QStringLiteral("The file names no architecture.")
                                            : QString("The architecture \"%1\" isn't supported (WaveNet, LSTM, Linear and ConvNet are).").arg(m.archName));
    if (!ok) return false;
    // the summary line for the panel
    switch (m.arch) {
    case Arch::WaveNet: {
        m.channelsSummary = m.arrays.front().channels;
        QStringList ch, counts;
        bool gated = false;
        for (const WaveArray& A : m.arrays) {
            ch << QString::number(A.channels);
            counts << QString::number(A.layers.size());
            for (const WaveLayer& L : A.layers) gated = gated || L.gating;
        }
        const bool same = std::all_of(counts.begin(), counts.end(), [&](const QString& c) { return c == counts.first(); });
        m.summary = QString("WaveNet · %1 channels · %2 layers%3").arg(ch.join('/'))
                        .arg(same && counts.size() > 1 ? QString("%1 × %2").arg(counts.size()).arg(counts.first()) : counts.join(" + "))
                        .arg(gated ? " · gated" : "");
        break;
    }
    case Arch::LSTM:    m.summary = QString("LSTM · %1 layer%2 × %3 units").arg(m.lstm.size()).arg(m.lstm.size() == 1 ? "" : "s").arg(m.lstm.front().hidden); break;
    case Arch::Linear:  m.summary = QString("Linear · %1 taps").arg(m.ir.size()); break;
    case Arch::ConvNet: m.summary = QString("ConvNet · %1 channels · %2 blocks").arg(m.blocks.front().conv.out).arg(m.blocks.size()); break;
    }
    return true;
}

} // namespace

namespace {

// One capture from a file's root object (or a container's submodel).
ModelPtr parseRoot(const QJsonObject& root, QString* error, const QString& path) {
    auto fail = [error](const QString& why) { if (error) *error = why; return ModelPtr(); };
    auto m = std::make_shared<Model>();
    m->path = path;
    m->fileName = QFileInfo(path).fileName();
    if (root.value("architecture").toString() == "SlimmableContainer") {
        // several sizes of one capture: the biggest is this model, the lighter ones its variants
        const QJsonArray subs = root.value("config").toObject().value("submodels").toArray();
        if (subs.isEmpty()) return fail("The container holds no captures.");
        std::vector<Variant> all;
        for (const QJsonValue& v : subs) {
            const QJsonObject entry = v.toObject();
            QString why;
            ModelPtr sub = parseRoot(entry.value("model").toObject(), &why, path);
            if (!sub) return fail("A size in the container can't be read: " + why);
            if (sub->container) return fail("A container inside a container isn't supported.");
            all.push_back({jnum(entry, "max_value", 1.0), std::move(sub)});
        }
        std::sort(all.begin(), all.end(), [](const Variant& a, const Variant& b) { return a.maxValue < b.maxValue; });
        const Model& full = *all.back().model;
        *m = full;                       // the network of the full size
        m->container = true;
        m->path = path;
        m->fileName = QFileInfo(path).fileName();
        m->variants.assign(all.begin(), all.end() - 1);
        m->version = root.value("version").toString();
        m->archName = full.archName;
        if (present(root, "sample_rate")) { m->sampleRate = root.value("sample_rate").toDouble(); m->rateKnown = m->sampleRate > 0; }
        for (const Variant& v : m->variants)
            if (v.model->rateKnown && m->rateKnown && std::fabs(v.model->sampleRate - m->sampleRate) > 0.5) return fail("The container's sizes were trained at different rates.");
        // the container's own metadata wins (the plugins read its loudness), the full size's fills in
        const QJsonObject meta = root.value("metadata").toObject();
        if (const QString n = meta.value("name").toString().trimmed(); !n.isEmpty()) m->name = n;
        else if (m->name.isEmpty() || m->name == QStringLiteral("Capture")) m->name = QFileInfo(path).completeBaseName();
        if (meta.value("loudness").isDouble()) { m->hasLoudness = true; m->loudness = meta.value("loudness").toDouble(); }
        if (meta.value("gain").isDouble()) { m->hasGain = true; m->gain = meta.value("gain").toDouble(); }
        if (meta.value("input_level_dbu").isDouble()) { m->hasInputLevel = true; m->inputLevel = meta.value("input_level_dbu").toDouble(); }
        if (meta.value("output_level_dbu").isDouble()) { m->hasOutputLevel = true; m->outputLevel = meta.value("output_level_dbu").toDouble(); }
        if (const QString by = meta.value("modeled_by").toString().trimmed(); !by.isEmpty()) m->modeledBy = by;
        QStringList gear, kind;
        for (const char* k : {"gear_make", "gear_model"}) if (const QString g = meta.value(QLatin1String(k)).toString().trimmed(); !g.isEmpty() && !gear.contains(g)) gear << g;
        for (const char* k : {"gear_type", "tone_type"}) if (const QString g = meta.value(QLatin1String(k)).toString().trimmed(); !g.isEmpty()) kind << g;
        if (!gear.isEmpty() || !kind.isEmpty())
            m->gear = gear.join(' ') + (kind.isEmpty() ? QString() : (gear.isEmpty() ? QString() : QStringLiteral(" ")) + "(" + kind.join(", ") + ")");
        QStringList sizes;
        for (const Variant& v : m->variants) sizes << (v.model->channelsSummary ? QString("%1 ch").arg(v.model->channelsSummary) : v.model->summary);
        m->summary += QString(" · %1 sizes (light: %2)").arg(all.size()).arg(sizes.join(", "));
        m->weightCount = 0;
        for (const Variant& v : all) m->weightCount += v.model->weightCount;
        return m;
    }
    const QJsonValue wv = root.value("weights");
    if (!wv.isArray()) return fail("It isn't a .nam capture (no weights).");
    std::vector<float> weights;
    const QJsonArray wa = wv.toArray();
    weights.reserve(size_t(wa.size()));
    for (const QJsonValue& v : wa) weights.push_back(float(v.toDouble()));
    m->weightCount = int(weights.size());
    Reader r;
    if (!build(r, root, &weights, *m)) return fail(r.error);
    if (r.cur.at != weights.size())
        return fail(QString("The weights don't fit the architecture: the file holds %1, it needs %2.").arg(weights.size()).arg(r.cur.at));
    for (float w : weights) if (!std::isfinite(w)) return fail("The weights hold a value that isn't a number.");
    return m;
}

} // namespace

ModelPtr parse(const QByteArray& json, QString* error, const QString& path) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (!doc.isObject()) { if (error) *error = QString("It isn't a .nam file (not JSON: %1).").arg(pe.errorString()); return nullptr; }
    return parseRoot(doc.object(), error, path);
}

namespace {
struct Cached { QDateTime modified; qint64 size = 0; ModelPtr model; QString error; };
QMutex& cacheMutex() { static QMutex m; return m; }
QHash<QString, Cached>& cache() { static QHash<QString, Cached> c; return c; }
}

ModelPtr load(const QString& path, QString* error) {
    if (path.isEmpty()) { if (error) *error = "No file."; return nullptr; }
    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();
    QMutexLocker lock(&cacheMutex());
    Cached& c = cache()[abs];
    if (!fi.exists()) { c = {}; if (error) *error = "The file is missing."; return nullptr; }
    if (c.model && c.modified == fi.lastModified() && c.size == fi.size()) return c.model;
    if (!c.model && !c.error.isEmpty() && c.modified == fi.lastModified() && c.size == fi.size()) { if (error) *error = c.error; return nullptr; }
    c = {fi.lastModified(), fi.size(), nullptr, {}};
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) { c.error = f.errorString(); if (error) *error = c.error; return nullptr; }
    if (f.size() > 64 * 1024 * 1024) { c.error = "The file is too big to be a capture."; if (error) *error = c.error; return nullptr; }
    c.model = parse(f.readAll(), &c.error, abs);
    if (!c.model && error) *error = c.error;
    return c.model;
}

bool isModelFile(const QString& path) { return QFileInfo(path).suffix().compare("nam", Qt::CaseInsensitive) == 0; }
QStringList nameFilters() { return {"*.nam"}; }

namespace {
QByteArray exampleJson(unsigned seed) {
    // a small two-array WaveNet (8 / 4 channels, ten layers each, like the trainer's "feather")
    auto layer = [](int inputSize, int channels, int headSize, bool headBias) {
        return QJsonObject{{"input_size", inputSize}, {"condition_size", 1}, {"head_size", headSize}, {"channels", channels}, {"kernel_size", 3},
                           {"dilations", QJsonArray{1, 2, 4, 8, 16, 32, 64, 128, 256, 512}}, {"activation", "Tanh"}, {"gated", false}, {"head_bias", headBias}};
    };
    QJsonObject root{{"version", "0.5.4"},
                     {"metadata", QJsonObject{{"name", "Sessions Example Capture"}, {"modeled_by", "Sessions"}, {"gear_type", "amp"}, {"gear_make", "Sessions"},
                                              {"gear_model", "Example"}, {"tone_type", "crunch"}, {"loudness", -20.0}, {"gain", 0.2}}},
                     {"architecture", "WaveNet"},
                     {"config", QJsonObject{{"layers", QJsonArray{layer(1, 8, 4, false), layer(8, 4, 1, true)}}, {"head", QJsonValue::Null}, {"head_scale", 0.02}}},
                     {"sample_rate", 48000}};
    Model m;
    Reader r;
    build(r, root, nullptr, m);   // counts the weights the shape needs
    QJsonArray weights;
    uint32_t s = 0x9e3779b9u ^ (seed * 2654435761u);
    for (size_t i = 0; i < r.cur.at; ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        weights.append(double(int32_t(s)) / 2147483648.0 * 0.35);
    }
    root.insert("weights", weights);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
} // namespace

bool writeExample(const QString& path, unsigned seed) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(exampleJson(seed)) >= 0 && f.commit();
}

// ============================================================================ running a channel


// Blocks are channel-major: block[c * n + t] for n frames of a chunk, so every inner loop runs
// over the frames of one channel, contiguous and without a reduction.

namespace {

// y[o][t] = bias[o] (+ y) + sum_i w[i][o] x[i][t]: a 1x1 over a block.
NAM_HOT void blockDense(const Conv& c, const float* x, float* y, int n, bool add = false) {
    const int in = c.in, out = c.out;
    for (int o = 0; o < out; ++o) {
        float* yo = y + size_t(o) * n;
        const float v = c.bias ? c.b[size_t(o)] : 0.0f;
        if (!add) std::fill(yo, yo + n, v);
        else if (c.bias) for (int t = 0; t < n; ++t) yo[t] += v;
    }
    const float* w = c.w.data();
    for (int i = 0; i < in; ++i) {
        const float* xi = x + size_t(i) * n;
        const float* wi = w + size_t(i) * out;
        for (int o = 0; o < out; ++o) {
            const float wo = wi[o];
            float* yo = y + size_t(o) * n;
            for (int t = 0; t < n; ++t) yo[t] += wo * xi[t];
        }
    }
}

// PReLU keeps one slope per channel; everything else is elementwise.
NAM_HOT void applyBlock(const Activation& a, float* x, int channels, int n) {
    if (a.type != Activation::PReLU || a.slopes.empty()) { a.apply(x, channels * n); return; }
    for (int c = 0; c < channels; ++c) {
        const float s = a.slopes[size_t(c) % a.slopes.size()];
        float* xc = x + size_t(c) * n;
        for (int t = 0; t < n; ++t) xc[t] = xc[t] > 0 ? xc[t] : s * xc[t];
    }
}

} // namespace

void Channel::Hist::init(int ch, int minFrames) {
    channels = ch;
    size = 1;
    while (size < minFrames) size <<= 1;
    mask = size - 1;
    data.assign(size_t(channels) * 2 * size_t(size), 0.0f);
    pos = 0;
}

void Channel::Hist::write(const float* block, int n) {
    for (int c = 0; c < channels; ++c) {
        float* d = &data[size_t(c) * 2 * size_t(size)];
        const float* x = block + size_t(c) * n;
        for (int t = 0; t < n; ++t) {
            const int i = (pos + t) & mask;
            d[i] = x[t];
            d[i + size] = x[t];
        }
    }
    pos = (pos + n) & mask;
}

void Channel::Hist::push(const float* frame) {
    for (int c = 0; c < channels; ++c) {
        float* d = &data[size_t(c) * 2 * size_t(size)];
        d[pos] = frame[c];
        d[pos + size] = frame[c];
    }
    pos = (pos + 1) & mask;
}

// y[o][t] = bias[o] + sum_k sum_i w[k][i][o] x[i][t - lag_k] over a chunk: `rows` points at the
// history run of every (tap, input channel), the frames of the chunk contiguous from there.
NAM_HOT static void blockConvImpl(const Conv& c, const float* const* rows, float* y, int n) {
    const int in = c.in, out = c.out;
    for (int o = 0; o < out; ++o) std::fill(y + size_t(o) * n, y + size_t(o) * n + n, c.bias ? c.b[size_t(o)] : 0.0f);
    const float* w = c.w.data();
    for (int k = 0; k < c.kernel; ++k) {
        const float* wk = w + size_t(k) * in * out;
        for (int i = 0; i < in; ++i) {
            const float* xi = rows[size_t(k) * in + i];
            const float* wi = wk + size_t(i) * out;
            for (int o = 0; o < out; ++o) {
                const float wo = wi[o];
                float* yo = y + size_t(o) * n;
                for (int t = 0; t < n; ++t) yo[t] += wo * xi[t];
            }
        }
    }
}

Channel::Channel(const Model& m) : m_m(m) {
    auto hist = [this](const Conv& c, int frames) { Hist h; h.init(c.in, c.lookback() + frames); m_hist.push_back(std::move(h)); };
    switch (m.arch) {
    case Arch::WaveNet:
        for (const WaveArray& A : m.arrays) {
            for (const WaveLayer& L : A.layers) hist(L.conv, kMaxFrames);
            hist(A.headRechannel, kMaxFrames);
        }
        for (const Conv& c : m.head.convs) hist(c, kMaxFrames);
        break;
    case Arch::LSTM:
        for (const LstmLayer& L : m.lstm) { m_h.insert(m_h.end(), L.h0.begin(), L.h0.end()); m_c.insert(m_c.end(), L.c0.begin(), L.c0.end()); }
        break;
    case Arch::Linear: {
        Hist h;
        h.init(1, int(m.ir.size()));
        m_hist.push_back(std::move(h));
        m_irRev.assign(m.ir.rbegin(), m.ir.rend());
        break;
    }
    case Arch::ConvNet: for (const ConvBlock& b : m.blocks) hist(b.conv, 1); break;
    }
    const size_t scratch = size_t(kMaxFrames) * kMaxChannels;
    m_a.assign(scratch, 0.0f); m_b.assign(scratch, 0.0f); m_z.assign(2 * scratch, 0.0f);
    m_acc.assign(scratch, 0.0f); m_headOut.assign(scratch, 0.0f); m_pre.assign(scratch, 0.0f);
    m_cond.assign(size_t(kMaxFrames), 0.0f);
}

void Channel::clear() {
    for (Hist& h : m_hist) { std::fill(h.data.begin(), h.data.end(), 0.0f); h.pos = 0; }
    size_t h = 0, c = 0;
    for (const LstmLayer& L : m_m.lstm) {
        std::copy(L.h0.begin(), L.h0.end(), m_h.begin() + long(h)); h += L.h0.size();
        std::copy(L.c0.begin(), L.c0.end(), m_c.begin() + long(c)); c += L.c0.size();
    }
}

void Channel::copyStateFrom(const Channel& o) {
    for (size_t i = 0; i < m_hist.size() && i < o.m_hist.size(); ++i) {
        Hist& h = m_hist[i];
        const Hist& s = o.m_hist[i];
        if (h.data.size() != s.data.size()) continue;
        std::copy(s.data.begin(), s.data.end(), h.data.begin());
        h.pos = s.pos;
    }
    std::copy(o.m_h.begin(), o.m_h.begin() + long(std::min(m_h.size(), o.m_h.size())), m_h.begin());
    std::copy(o.m_c.begin(), o.m_c.begin() + long(std::min(m_c.size(), o.m_c.size())), m_c.begin());
}

void Channel::process(const float* in, float* out, int n) {
    for (int pos = 0; pos < n; pos += kMaxFrames) chunk(in + pos, out + pos, std::min(kMaxFrames, n - pos));
}

void Channel::chunk(const float* in, float* out, int n) {
    std::copy(in, in + n, m_cond.begin());   // in and out may be the same buffer
    switch (m_m.arch) {
    case Arch::WaveNet: waveNet(m_cond.data(), out, n); break;
    case Arch::LSTM:    lstm(m_cond.data(), out, n); break;
    case Arch::Linear:  linear(m_cond.data(), out, n); break;
    case Arch::ConvNet: convNet(m_cond.data(), out, n); break;
    }
}

void Channel::waveNet(const float* in, float* out, int n) {
    const Model& m = m_m;
    float* cur = m_a.data();
    float* nxt = m_b.data();
    float* z = m_z.data();
    float* acc = m_acc.data();
    float* headOut = m_headOut.data();
    const float* rows[kMaxKernel * kMaxChannels];
    const float* x = in;   // the first array's input: the sound itself (one channel)
    int headWidth = 0;
    size_t hi = 0;
    // a conv over the chunk, its frames written to `h` first
    auto conv = [&](const Conv& c, Hist& h, const float* block, float* y) {
        const int from = h.pos;
        h.write(block, n);
        for (int k = 0; k < c.kernel; ++k) {
            const int lag = (c.kernel - 1 - k) * c.dilation;
            for (int i = 0; i < c.in; ++i) rows[size_t(k) * c.in + i] = h.run(i, lag, from);
        }
        blockConvImpl(c, rows, y, n);
    };
    for (size_t a = 0; a < m.arrays.size(); ++a) {
        const WaveArray& A = m.arrays[a];
        blockDense(A.rechannel, x, nxt, n);
        std::swap(cur, nxt);
        x = cur;
        const int width = A.channels;
        if (a == 0) std::fill(acc, acc + size_t(A.headIn) * n, 0.0f);
        else std::copy(headOut, headOut + size_t(A.headIn) * n, acc);   // headIn == the head size before
        for (const WaveLayer& L : A.layers) {
            const int b = L.bottleneck;
            conv(L.conv, m_hist[hi++], x, z);
            blockDense(L.mixin, in, z, n, true);
            float* top = z;
            if (L.gating == 0) {
                applyBlock(L.act, z, b, n);
            } else if (L.gating == 1) {
                applyBlock(L.act, z, b, n);
                applyBlock(L.act2, z + size_t(b) * n, b, n);
                for (int e = 0; e < b * n; ++e) z[e] *= z[size_t(b) * n + e];
            } else {
                float* pre = m_pre.data();
                std::copy(z, z + size_t(b) * n, pre);
                applyBlock(L.act, z, b, n);
                applyBlock(L.act2, z + size_t(b) * n, b, n);
                for (int e = 0; e < b * n; ++e) { const float al = z[size_t(b) * n + e]; z[e] = al * z[e] + (1.0f - al) * pre[e]; }
            }
            if (L.hasHead1x1) blockDense(L.head1x1, top, acc, n, true);
            else for (int e = 0; e < b * n; ++e) acc[e] += top[e];
            if (L.hasLayer1x1) {
                blockDense(L.layer1x1, top, nxt, n);
                for (int e = 0; e < width * n; ++e) nxt[e] += x[e];
                std::swap(cur, nxt);
                x = cur;
            }
        }
        conv(A.headRechannel, m_hist[hi++], acc, headOut);
        headWidth = A.headSize;
    }
    if (!m.hasHead) {
        for (int t = 0; t < n; ++t) out[t] = m.headScale * headOut[t];
        return;
    }
    float* hx = nxt;
    for (int e = 0; e < headWidth * n; ++e) hx[e] = m.headScale * headOut[e];
    float* hy = z;
    for (size_t i = 0; i < m.head.convs.size(); ++i) {
        const Conv& c = m.head.convs[i];
        applyBlock(m.head.act, hx, c.in, n);
        conv(c, m_hist[hi++], hx, hy);
        std::swap(hx, hy);
    }
    std::copy(hx, hx + n, out);
}

void Channel::lstm(const float* in, float* out, int n) {
    const Model& m = m_m;
    for (int t = 0; t < n; ++t) {
        float x[kMaxChannels];
        int xw = 1;
        x[0] = in[t];
        size_t ho = 0;
        for (const LstmLayer& L : m.lstm) {
            const int H = L.hidden, rows = 4 * H;
            float* h = m_h.data() + ho;
            float* c = m_c.data() + ho;
            float ifgo[4 * kMaxChannels];
            std::copy(L.b.begin(), L.b.end(), ifgo);
            const float* w = L.w.data();
            for (int j = 0; j < xw; ++j) { const float v = x[j]; const float* wj = w + size_t(j) * rows; for (int g = 0; g < rows; ++g) ifgo[g] += v * wj[g]; }
            for (int j = 0; j < H; ++j) { const float v = h[j]; const float* wj = w + size_t(xw + j) * rows; for (int g = 0; g < rows; ++g) ifgo[g] += v * wj[g]; }
            for (int k = 0; k < H; ++k) {
                c[k] = sigmoid(ifgo[H + k]) * c[k] + sigmoid(ifgo[k]) * fastTanh(ifgo[2 * H + k]);
                h[k] = sigmoid(ifgo[3 * H + k]) * fastTanh(c[k]);
            }
            std::copy(h, h + H, x);
            xw = H;
            ho += size_t(H);
        }
        float y = m.lstmHead.b[0];
        for (int j = 0; j < xw; ++j) y += m.lstmHead.w[size_t(j)] * x[j];
        out[t] = y;
    }
}

void Channel::linear(const float* in, float* out, int n) {
    const Model& m = m_m;
    Hist& h = m_hist[0];
    const int taps = int(m.ir.size());
    for (int t = 0; t < n; ++t) {
        h.push(in + t);
        const float* x = h.run(0, taps, h.pos);   // the last `taps` samples, oldest first
        float y = m.irBias;
        for (int j = 0; j < taps; ++j) y += m_irRev[size_t(j)] * x[j];
        out[t] = y;
    }
}

void Channel::convNet(const float* in, float* out, int n) {
    const Model& m = m_m;
    for (int t = 0; t < n; ++t) {
        float x[kMaxChannels], y[kMaxChannels];
        int xw = 1;
        x[0] = in[t];
        for (size_t i = 0; i < m.blocks.size(); ++i) {
            const ConvBlock& b = m.blocks[i];
            const Conv& c = b.conv;
            Hist& h = m_hist[i];
            h.push(x);
            for (int o = 0; o < c.out; ++o) y[o] = c.bias ? c.b[size_t(o)] : 0.0f;
            for (int k = 0; k < c.kernel; ++k) {
                const int lag = (c.kernel - 1 - k) * c.dilation;
                const float* wk = c.w.data() + size_t(k) * c.in * c.out;
                for (int ci = 0; ci < c.in; ++ci) {
                    const float xi = h.at(ci, lag);
                    const float* wi = wk + size_t(ci) * c.out;
                    for (int o = 0; o < c.out; ++o) y[o] += xi * wi[o];
                }
            }
            if (b.batchnorm) for (int o = 0; o < c.out; ++o) y[o] = y[o] * b.scale[size_t(o)] + b.shift[size_t(o)];
            b.act.apply(y, c.out);
            xw = c.out;
            std::copy(y, y + xw, x);
        }
        float v = m.convHead.b[0];
        for (int j = 0; j < xw; ++j) v += m.convHead.w[size_t(j)] * x[j];
        out[t] = v;
    }
}

// ============================================================================ resampling

// A windowed-sinc interpolator for one direction (32 taps, 64 sub-sample phases, linear
// between them), cut below the lower of the two Nyquists. It keeps a short history and
// hands out as many output samples as the input so far allows.
class Runtime::Resampler {
public:
    static constexpr int kTaps = 32, kHalf = 16, kPhases = 64;
    void init(double inRate, double outRate, int maxIn) {
        m_step = inRate / outRate;
        const double fc = 0.45 * std::min(1.0, outRate / inRate);   // cycles per input sample
        m_table.assign(size_t(kPhases + 1) * kTaps, 0.0f);
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = double(p) / kPhases;
            double sum = 0;
            for (int t = 0; t < kTaps; ++t) {
                const double x = t - kHalf + 1 - frac;   // tap position relative to the output's time
                const double sinc = std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
                const double u = (x + kHalf) / kTaps;      // 0..1 across the window
                const double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * u) + 0.14128 * std::cos(4 * M_PI * u) - 0.01168 * std::cos(6 * M_PI * u);
                m_table[size_t(p) * kTaps + size_t(t)] = float(sinc * w);
                sum += sinc * w;
            }
            for (int t = 0; t < kTaps; ++t) m_table[size_t(p) * kTaps + size_t(t)] = float(m_table[size_t(p) * kTaps + size_t(t)] / sum);
        }
        m_hist.assign(size_t(kTaps + maxIn + 8), 0.0f);
        reset();
    }
    void reset() {
        std::fill(m_hist.begin(), m_hist.end(), 0.0f);
        m_filled = kHalf;    // silence before the first sample
        m_pos = kHalf;       // the first output is at the first input sample
    }
    int process(const float* in, int n, float* out, int maxOut) {
        std::copy(in, in + n, m_hist.begin() + m_filled);
        m_filled += n;
        int made = 0;
        while (made < maxOut) {
            const int idx = int(std::floor(m_pos));
            if (idx + kHalf >= m_filled) break;
            const double ph = (m_pos - idx) * kPhases;
            const int p = std::min(kPhases - 1, int(ph));
            const float pf = float(ph - p);
            const float* h0 = &m_table[size_t(p) * kTaps];
            const float* h1 = h0 + kTaps;
            const float* x = &m_hist[size_t(idx - kHalf + 1)];
            float acc = 0;
            for (int t = 0; t < kTaps; ++t) acc += x[t] * (h0[t] + (h1[t] - h0[t]) * pf);
            out[made++] = acc;
            m_pos += m_step;
        }
        const int keepFrom = std::max(0, int(std::floor(m_pos)) - kHalf + 1);
        if (keepFrom > 0) {
            std::copy(m_hist.begin() + keepFrom, m_hist.begin() + m_filled, m_hist.begin());
            m_filled -= keepFrom;
            m_pos -= keepFrom;
        }
        return made;
    }
    void copyFrom(const Resampler& o) {
        std::copy(o.m_hist.begin(), o.m_hist.begin() + long(std::min(m_hist.size(), o.m_hist.size())), m_hist.begin());
        m_filled = o.m_filled; m_pos = o.m_pos;
    }
private:
    std::vector<float> m_table, m_hist;
    int    m_filled = 0;
    double m_pos = 0, m_step = 1;
};

// One side's resampling state: up to the capture's rate, the network's block, down again,
// and a FIFO that hands out exactly n frames a block.
struct Runtime::Side {
    Resampler up, down;
    std::vector<float> work, made, fifo;
    int fifoRead = 0, fifoCount = 0, prime = 0;
    void reset() {
        up.reset(); down.reset();
        std::fill(fifo.begin(), fifo.end(), 0.0f);
        fifoRead = 0; fifoCount = prime;
    }
    void copyFrom(const Side& o) {
        up.copyFrom(o.up); down.copyFrom(o.down);
        std::copy(o.fifo.begin(), o.fifo.begin() + long(std::min(fifo.size(), o.fifo.size())), fifo.begin());
        fifoRead = o.fifoRead; fifoCount = o.fifoCount;
    }
};

Runtime::Runtime(ModelPtr model, float engineRate) : m_model(std::move(model)), m_engineRate(engineRate) {
    const Model& m = *m_model;
    const double ratio = m.sampleRate / double(engineRate);   // network samples per engine sample
    m_resampling = std::fabs(m.sampleRate - engineRate) > 0.5 && ratio <= 4.0 && ratio >= 0.25;
    m_left = std::make_unique<Channel>(m);
    m_right = std::make_unique<Channel>(m);
    m_warm = std::make_unique<Channel>(m);
    if (m_resampling) {
        const int cap = int(audio::kMaxBlock * std::max(1.0, ratio)) + 16;
        for (auto* s : {&m_sideL, &m_sideR}) {
            *s = std::make_unique<Side>();
            (*s)->up.init(engineRate, m.sampleRate, audio::kMaxBlock);
            (*s)->down.init(m.sampleRate, engineRate, cap);
            (*s)->work.assign(size_t(cap), 0.0f);
            (*s)->made.assign(size_t(cap), 0.0f);
            (*s)->prime = Resampler::kHalf + int(std::ceil(Resampler::kHalf / ratio)) + 8;
            (*s)->fifo.assign(size_t((*s)->prime + 4 * audio::kMaxBlock + cap), 0.0f);
            (*s)->reset();
        }
        m_latency = m_sideL->prime;
    }
    // prewarm: silence through the receptive field settles the network's state (its biases)
    const int warm = std::clamp(m.receptiveField, 1, int(m.sampleRate * 2));
    float zeros[kMaxFrames] = {}, sink[kMaxFrames];
    for (int done = 0; done < warm; done += kMaxFrames) m_left->process(zeros, sink, std::min(kMaxFrames, warm - done));
    m_warm->copyStateFrom(*m_left);
    m_right->copyStateFrom(*m_left);
    m_linked = true;
}

Runtime::~Runtime() = default;

void Runtime::reset() {
    m_left->copyStateFrom(*m_warm);
    if (m_sideL) m_sideL->reset();
    m_linked = true;
}

void Runtime::run(Channel& ch, Side* side, float* x, int n) {
    if (!m_resampling || !side) { ch.process(x, x, n); return; }
    Side& s = *side;
    const int cap = int(s.work.size());
    const int k = s.up.process(x, n, s.work.data(), cap);
    ch.process(s.work.data(), s.work.data(), k);
    const int made = s.down.process(s.work.data(), k, s.made.data(), cap);
    const int fifoSize = int(s.fifo.size());
    for (int i = 0; i < made && s.fifoCount < fifoSize; ++i) {
        s.fifo[size_t((s.fifoRead + s.fifoCount) % fifoSize)] = s.made[size_t(i)];
        ++s.fifoCount;
    }
    for (int i = 0; i < n; ++i) {
        if (s.fifoCount > 0) { x[i] = s.fifo[size_t(s.fifoRead)]; s.fifoRead = (s.fifoRead + 1) % fifoSize; --s.fifoCount; }
        else x[i] = 0.0f;
    }
}

void Runtime::process(float* L, float* R, int n) {
    const bool same = std::equal(L, L + n, R);
    if (m_linked && !same) {   // the sides part: the right one takes over the left one's history
        m_right->copyStateFrom(*m_left);
        if (m_sideR) m_sideR->copyFrom(*m_sideL);
        m_linked = false;
    }
    run(*m_left, m_sideL.get(), L, n);
    if (m_linked) std::copy(L, L + n, R);
    else run(*m_right, m_sideR.get(), R, n);
}

} // namespace sessions::nam

// ============================================================================ the NAM device

namespace sessions::audio {

namespace {

using namespace kit;

struct DcBlock {   // a 5 Hz high-pass: captures can carry a small offset, which would eat headroom
    float x1 = 0, y1 = 0, r = 0.9993f;
    void set(float sr) { r = std::exp(-kTwoPi * 5.0f / sr); }
    float process(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

inline float peakOf(const float* L, const float* R, int n) {
    float p = 0;
    for (int i = 0; i < n; ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
}

// Input trim, a noise gate, the capture (Context::nam, the engine's prepared Runtime, dry when
// there's none), a DC blocker, Normalize (the capture's loudness metadata to -18 dB), the IR slot (Context::ir,
// like the amp's Loaded IR, with its own mix), Output trim, Mix. meter[0] / [1]: in / out peaks.
class NamDevice : public DeviceProcessor {
public:
    explicit NamDevice(float sr) : m_sr(sr) { m_dc[0].set(sr); m_dc[1].set(sr); }
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        meter[0].store(peakOf(L, R, n), std::memory_order_relaxed);
        const float inGain = dbToGain(pv(T::Nam, p, namdev::Input));
        const float outGain = dbToGain(pv(T::Nam, p, namdev::Output));
        const float mix = p[namdev::Mix];
        const float gateDb = pv(T::Nam, p, namdev::Gate);
        const bool  gateOn = gateDb > -89.0f;   // all the way down: off
        const bool  normalize = pb(p, namdev::Normalize), irOn = pb(p, namdev::Ir);
        const float irMix = p[namdev::IrMix];
        nam::Runtime* rt = ctx.nam;
        if (m_resetPending) { if (rt) rt->reset(); m_resetPending = false; }
        const IrKernel* ir = irOn ? ctx.ir : nullptr;
        if (ir != m_irNow) { m_irNow = ir; m_conv[0].reset(); m_conv[1].reset(); }
        float normDb = 0;
        if (normalize && rt && rt->model()->hasLoudness) normDb = std::clamp(float(namdev::kNormalizeTargetDb - rt->model()->loudness), -24.0f, 24.0f);
        const float coef = onePoleCoef(0.01f, m_sr);
        const float thr = gateOn ? dbToGain(gateDb) : 0.0f;
        const float envAtt = onePoleCoef(0.0005f, m_sr), envRel = onePoleCoef(0.02f, m_sr);
        const float gateOpen = onePoleCoef(0.002f, m_sr), gateClose = onePoleCoef(0.02f, m_sr);
        for (int i = 0; i < n; ++i) {
            m_dryL[i] = L[i]; m_dryR[i] = R[i];
            const float g = m_in.next(inGain, coef);
            float x = L[i] * g, y = R[i] * g;
            // the gate: one detector on the louder side, so a mono input keeps both sides identical
            const float level = std::max(std::fabs(x), std::fabs(y));
            m_env = level + (m_env - level) * (level > m_env ? envAtt : envRel);
            if (gateOn) {
                if (m_env > thr) m_gateOpen = true;
                else if (m_env < thr * 0.5f) m_gateOpen = false;   // 6 dB of hysteresis
            } else m_gateOpen = true;
            const float target = m_gateOpen ? 1.0f : 0.0f;
            m_gate = target + (m_gate - target) * (target > m_gate ? gateOpen : gateClose);
            m_wetL[i] = x * m_gate; m_wetR[i] = y * m_gate;
        }
        if (rt) {
            rt->process(m_wetL, m_wetR, n);
            for (int i = 0; i < n; ++i) { m_wetL[i] = m_dc[0].process(m_wetL[i]); m_wetR[i] = m_dc[1].process(m_wetR[i]); }
        }
        for (int i = 0; i < n; ++i) {
            const float g = m_norm.next(dbToGain(normDb), coef);
            m_wetL[i] *= g; m_wetR[i] *= g;
        }
        // Host tone controls after the capture, before the cabinet. Neutral values preserve old sets.
        float gains[4]; bool changed = false;
        for (int b = 0; b < 4; ++b) {
            const float target = pv(T::Nam, p, namdev::Bass + b);
            gains[b] = m_toneGain[b].next(target, std::exp(-float(n) / (.015f * m_sr)));
            changed |= gains[b] != m_toneLast[b]; m_toneLast[b] = gains[b];
        }
        if (changed) for (int c = 0; c < 2; ++c) {
            m_tone[c][0].lowShelf(std::min(120.f,m_sr*.4f),gains[0],m_sr);
            m_tone[c][1].peak(std::min(750.f,m_sr*.4f),.7f,gains[1],m_sr);
            m_tone[c][2].highShelf(std::min(2200.f,m_sr*.4f),gains[2],m_sr);
            m_tone[c][3].highShelf(std::min(4800.f,m_sr*.4f),gains[3],m_sr);
        }
        for (int i = 0; i < n; ++i) for (int b = 0; b < 4; ++b) {
            m_wetL[i] = m_tone[0][b].process(m_wetL[i]); m_wetR[i] = m_tone[1][b].process(m_wetR[i]);
        }
        if (ir) {   // the cabinet, one block of latency (see Convolver)
            float* wet[2] = {m_wetL, m_wetR};
            for (int c = 0; c < 2; ++c) {
                m_conv[c].process(*ir, ir->stereo ? c : 0, wet[c], m_tmp, n);
                for (int i = 0; i < n; ++i) wet[c][i] = wet[c][i] * (1 - irMix) + m_tmp[i] * irMix;
            }
        }
        for (int i = 0; i < n; ++i) {
            const float g = m_out.next(outGain, coef) * mix;
            L[i] = m_dryL[i] * (1 - mix) + m_wetL[i] * g;
            R[i] = m_dryR[i] * (1 - mix) + m_wetR[i] * g;
        }
        meter[1].store(peakOf(L, R, n), std::memory_order_relaxed);
    }
    void panic() override {
        m_resetPending = true;   // the runtime belongs to the render state: reset it on the next block
        m_conv[0].reset(); m_conv[1].reset();
        m_dc[0] = DcBlock(); m_dc[1] = DcBlock(); m_dc[0].set(m_sr); m_dc[1].set(m_sr);
        m_env = 0; m_gate = 0; m_gateOpen = false;
        for (auto& channel : m_tone) for (auto& band : channel) band.reset();
    }
private:
    float m_sr;
    Smoothed m_in, m_out, m_norm;
    Biquad m_tone[2][4]; Smoothed m_toneGain[4]; float m_toneLast[4] = {999,999,999,999};
    float m_env = 0, m_gate = 0;
    bool  m_gateOpen = false, m_resetPending = false;
    Convolver m_conv[2];
    DcBlock m_dc[2];
    const IrKernel* m_irNow = nullptr;
    float m_dryL[kMaxBlock] = {}, m_dryR[kMaxBlock] = {}, m_wetL[kMaxBlock] = {}, m_wetR[kMaxBlock] = {}, m_tmp[kMaxBlock] = {};
};

} // namespace

std::shared_ptr<DeviceProcessor> createNam(float sr) { return std::make_shared<NamDevice>(sr); }

} // namespace sessions::audio
