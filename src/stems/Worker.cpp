#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include "ModelAssets.h"
#include "StemsDsp.h"
#include "WaveIO.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QSaveFile>
#include <cstdio>
#include <memory>
#include <stdexcept>
using namespace sessions::stems;
namespace {
void message(const QString& phase, double progress, const QString& detail = {}) {
    const QByteArray data = QJsonDocument(QJsonObject{{"schema", 1}, {"phase", phase}, {"progress", progress}, {"detail", detail}}).toJson(QJsonDocument::Compact);
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout); std::fputc('\n', stdout); std::fflush(stdout);
}
void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Q_INIT_RESOURCE(worker_resources);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({QCommandLineOption("input", "Prepared WAV", "file"), QCommandLineOption("output", "Output folder", "folder"),
                       QCommandLineOption("models", "Spleeter model folder", "folder"), QCommandLineOption("runtime", "ONNX Runtime library", "file"),
                       QCommandLineOption("threads", "CPU threads", "count", "2")});
    parser.process(app);
    try {
        QString why;
        if (!verifyModelFiles(parser.value("models"), &why)) throw std::runtime_error(why.toStdString());
        const QString output = parser.value("output");
        require(QDir(output).exists(), "The output folder does not exist.");
        const int threads = std::clamp(parser.value("threads").toInt(), 1, 8);
        message("loading", 0, "Loading neural separation engine");
        QLibrary library(parser.value("runtime"));
        if (!library.load()) throw std::runtime_error(("Couldn't load ONNX Runtime: " + library.errorString()).toStdString());
        using GetApiBase = const OrtApiBase* (ORT_API_CALL*)();
        auto getApi = reinterpret_cast<GetApiBase>(library.resolve("OrtGetApiBase"));
        require(getApi != nullptr, "The neural runtime has no C API.");
        const OrtApi* api = getApi()->GetApi(ORT_API_VERSION);
        require(api != nullptr, "The neural runtime version does not match this worker.");
        Ort::InitApi(api);
        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "Sessions Stems Creator");
        env.DisableTelemetryEvents();
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(threads); options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // Five graphs share the process, but only one runs at once. Do not retain five
        // independent activation arenas for a long file.
        options.DisableCpuMemArena(); options.DisableMemPattern();
        std::vector<Ort::Session> models;
        Ort::AllocatorWithDefaultOptions allocator;
        const std::vector<int64_t> shape{1, 2, kModelFrames, kModelBins};
        for (const auto& asset : modelAssets()) {
            const QString file = QDir(parser.value("models")).filePath(asset.file);
#ifdef Q_OS_WIN
            const std::wstring name = file.toStdWString();
#else
            const std::string name = QFile::encodeName(file).toStdString();
#endif
            models.emplace_back(env, name.c_str(), options);
            const auto& model = models.back();
            require(model.GetInputCount() == 1 && model.GetOutputCount() == 1, "Unsupported model input/output count.");
            require(std::string(model.GetInputNameAllocated(0, allocator).get()) == "x"
                    && std::string(model.GetOutputNameAllocated(0, allocator).get()) == "y", "Unsupported model tensor names.");
            require(model.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape() == shape
                    && model.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape() == shape,
                    "Unsupported model tensor shape.");
            message("loading", .03 * models.size() / kSources, asset.name);
        }
        auto input = readInputWave(parser.value("input"));
        const qint64 samples = qint64(input.size() / 2);
        constexpr int pad = kFft - kHop, bins = kFft / 2 + 1;
        const int frames = int((samples + pad + kHop - 1) / kHop);
        constexpr int context = 64, stride = kModelFrames - context * 2;
        std::vector<int> starts{0};
        while (starts.back() + kModelFrames < frames) starts.push_back(starts.back() + stride);
        const auto window = hannWindow();
        const size_t tensorSize = size_t(2 * kModelFrames * kModelBins);
        std::vector<float> magnitude(tensorSize, 0);
        std::vector<Complex> spectra(size_t(kModelFrames) * 2 * bins), transform(kFft);
        std::array<std::vector<float>, kSources> logits;
        std::array<std::vector<float>, kSources> ring;
        std::vector<float> normal(kFft, 0);
        std::array<std::unique_ptr<FloatWave>, kSources> writers;
        for (int s = 0; s < kSources; ++s) {
            ring[s].resize(kFft * 2, 0);
            writers[s] = std::make_unique<FloatWave>(QDir(output).filePath(modelAssets()[s].name + ".wav"));
        }
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* names[] = {"x"}; const char* targets[] = {"y"};
        for (int block = 0; block < int(starts.size()); ++block) {
            const int start = starts[block];
            std::fill(magnitude.begin(), magnitude.end(), 0);
            for (int t = 0; t < kModelFrames; ++t) for (int c = 0; c < 2; ++c) {
                for (int n = 0; n < kFft; ++n) {
                    const qint64 index = qint64(start + t) * kHop + n - pad;
                    transform[n] = index >= 0 && index < samples ? input[size_t(index) * 2 + c] * window[n] : 0.f;
                }
                fft(transform);
                for (int f = 0; f < bins; ++f) spectra[(size_t(t) * 2 + c) * bins + f] = transform[f];
                for (int f = 0; f < kModelBins; ++f) magnitude[(size_t(c) * kModelFrames + t) * kModelBins + f] = std::abs(transform[f]);
            }
            auto tensor = Ort::Value::CreateTensor<float>(memory, magnitude.data(), magnitude.size(), shape.data(), shape.size());
            for (int s = 0; s < kSources; ++s) {
                message("separating", .03 + .94 * (block * kSources + s) / (starts.size() * kSources), modelAssets()[s].name);
                auto values = models[s].Run(Ort::RunOptions{nullptr}, names, &tensor, 1, targets, 1);
                const auto info = values[0].GetTensorTypeAndShapeInfo();
                require(info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT && info.GetShape() == shape, "Invalid model result.");
                const float* data = values[0].GetTensorData<float>();
                logits[s].assign(data, data + tensorSize);
            }
            // Central regions have context on either side; only the true file edges use
            // edge predictions. Each STFT frame contributes once to the common WOLA clock.
            const int first = block ? context : 0;
            const int last = block + 1 == int(starts.size()) ? std::min(kModelFrames, frames - start) : kModelFrames - context;
            std::array<std::vector<float>, kSources> masks;
            for (auto& m : masks) m.resize(bins);
            for (int t = first; t < last; ++t) {
                const qint64 at = qint64(start + t) * kHop;
                for (int c = 0; c < 2; ++c) {
                    std::array<float, kSources> high{};
                    for (int f = 0; f < kModelBins; ++f) {
                        std::array<float, kSources> raw;
                        for (int s = 0; s < kSources; ++s) raw[s] = logits[s][(size_t(c) * kModelFrames + t) * kModelBins + f];
                        const auto weights = ratioMasks(raw);
                        for (int s = 0; s < kSources; ++s) { masks[s][f] = weights[s]; high[s] += weights[s] / kModelBins; }
                    }
                    for (int s = 0; s < kSources; ++s) {
                        for (int f = kModelBins; f < bins; ++f) masks[s][f] = high[s];
                        for (int f = 0; f < bins; ++f) transform[f] = spectra[(size_t(t) * 2 + c) * bins + f] * masks[s][f];
                        transform[0].imag(0); transform[kFft / 2].imag(0);
                        for (int f = 1; f < kFft / 2; ++f) transform[kFft - f] = std::conj(transform[f]);
                        fft(transform, true);
                        for (int n = 0; n < kFft; ++n) ring[s][size_t((at + n) % kFft) * 2 + c] += transform[n].real() * window[n];
                    }
                }
                for (int n = 0; n < kFft; ++n) normal[size_t((at + n) % kFft)] += window[n] * window[n];
                for (int n = 0; n < kHop; ++n) {
                    const int slot = int((at + n) % kFft);
                    const qint64 source = at + n - pad;
                    for (int s = 0; s < kSources; ++s) {
                        if (source >= 0 && source < samples) {
                            require(normal[slot] > 1e-6f, "Invalid overlap-add normalization.");
                            writers[s]->add(ring[s][slot * 2] / normal[slot], ring[s][slot * 2 + 1] / normal[slot]);
                        }
                        ring[s][slot * 2] = ring[s][slot * 2 + 1] = 0;
                    }
                    normal[slot] = 0;
                }
            }
        }
        QJsonArray stems;
        for (int s = 0; s < kSources; ++s) {
            writers[s]->finish(samples);
            stems.push_back(QJsonObject{{"name", modelAssets()[s].name}, {"file", modelAssets()[s].name + ".wav"}});
        }
        QSaveFile manifest(QDir(output).filePath("result.json"));
        const QByteArray json = QJsonDocument(QJsonObject{{"schema", 1}, {"model", "spleeter-5stems-onnx-v1"},
            {"rate", 44100}, {"frames", double(samples)}, {"stems", stems}}).toJson();
        require(manifest.open(QIODevice::WriteOnly) && manifest.write(json) == json.size() && manifest.commit(), "Couldn't finish the stem manifest.");
        message("complete", 1);
        return 0;
    } catch (const std::exception& error) {
        message("error", 0, QString::fromUtf8(error.what()));
        return 1;
    }
}
