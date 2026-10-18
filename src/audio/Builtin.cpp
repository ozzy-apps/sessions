#include "Builtin.h"
#include "Devices.h"
#include "core/DeviceSpecs.h"
#include "core/Model.h"
#include <QHash>
#include <cmath>

namespace sessions::audio {

namespace {

constexpr float kRate = 48000;
const QString kPrefix = QStringLiteral("builtin:");

const char* soundName(int note) {
    switch (note) {
    case 36: return "Kick";      case 37: return "Rim";        case 38: return "Snare";      case 39: return "Clap";
    case 40: return "Snare 2";   case 41: return "Tom Low";    case 42: return "Hat Closed"; case 43: return "Tom Low 2";
    case 44: return "Hat Pedal"; case 45: return "Tom Mid";    case 46: return "Hat Open";   case 47: return "Tom Mid 2";
    case 48: return "Tom High";  case 49: return "Crash";      case 50: return "Tom High 2"; case 51: return "Ride";
    default: return nullptr;
    }
}

DeviceType kitType(const QString& kit) {
    if (kit == "808") return DeviceType::Drum808;
    if (kit == "acoustic") return DeviceType::DrumAcoustic;
    if (kit == "kit") return DeviceType::DrumKit;
    return DeviceType::Unknown;
}

// Plays one note through a fresh kit processor until it has died away (at most 4 s).
SamplePtr render(DeviceType type, int note, const QString& path) {
    auto proc = createProcessor(type, kRate);
    const DeviceSpec* spec = specOf(type);
    if (!proc || !spec) return nullptr;
    float params[kMaxParams] = {};
    for (size_t i = 0; i < spec->params.size() && i < size_t(kMaxParams); ++i) params[i] = float(spec->params[i].def);
    Context ctx;
    ctx.sampleRate = kRate;
    ctx.beatsPerSample = 2.0 / kRate;
    EventList ev;
    ev.add({0, note, 0.9f, int(0.1 * kRate)});
    std::vector<float> left, right;
    float l[kMaxBlock], r[kMaxBlock];
    int quietBlocks = 0;
    for (int done = 0; done < int(4 * kRate); done += kMaxBlock) {
        std::fill(l, l + kMaxBlock, 0.0f);
        std::fill(r, r + kMaxBlock, 0.0f);
        proc->render(ev, l, r, kMaxBlock, params, ctx);
        ev.clear();
        float peak = 0;
        for (int i = 0; i < kMaxBlock; ++i) peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
        left.insert(left.end(), l, l + kMaxBlock);
        right.insert(right.end(), r, r + kMaxBlock);
        quietBlocks = peak < 1e-4f ? quietBlocks + 1 : 0;
        if (done > int(0.05 * kRate) && quietBlocks >= 8) break;   // ~20 ms of silence
    }
    while (left.size() > 64 && std::fabs(left.back()) < 1e-5f && std::fabs(right.back()) < 1e-5f) { left.pop_back(); right.pop_back(); }
    return samples::fromFrames(std::move(left), std::move(right), kRate, path);
}

} // namespace

bool isBuiltin(const QString& path) { return path.startsWith(kPrefix); }
bool isBuiltinIr(const QString& path) { return path.startsWith(kPrefix + "ir/"); }

namespace {
const char* kCabNames[] = {"", "1x12 Open", "2x12 Combo", "4x12 Closed", "4x12 Vintage", "8x10 Bass"};
const char* kMicNames[] = {"On Axis", "Off Axis", "Far"};
}

QStringList builtinIrPaths() {
    QStringList out;
    for (int cab = 1; cab <= 5; ++cab)
        for (int mic = 0; mic < 3; ++mic) out << QString("%1ir/%2/%3").arg(kPrefix).arg(cab).arg(mic);
    return out;
}

QString builtinName(const QString& path) {
    if (isBuiltinIr(path)) {
        const int cab = path.section('/', 1, 1).toInt(), mic = path.section('/', 2, 2).toInt();
        if (cab >= 1 && cab <= 5 && mic >= 0 && mic <= 2) return QString("%1 · %2").arg(kCabNames[cab], kMicNames[mic]);
        return path.mid(kPrefix.size());
    }
    const int note = path.section('/', -1).toInt();
    const char* n = soundName(note);
    return n ? QString(n) : path.mid(kPrefix.size());
}

SamplePtr builtinSample(const QString& path) {
    static QHash<QString, SamplePtr> cache;   // UI thread only
    if (!isBuiltin(path)) return nullptr;
    if (const SamplePtr hit = cache.value(path)) return hit;
    if (isBuiltinIr(path)) {
        const int cab = path.section('/', 1, 1).toInt(), mic = path.section('/', 2, 2).toInt();
        if (cab < 1 || cab > 5 || mic < 0 || mic > 2) return nullptr;
        std::vector<float> ir = cabinetImpulse(cab, mic, kRate, 8192);
        SamplePtr s = samples::fromFrames(ir, {}, kRate, path);
        if (s) cache.insert(path, s);
        return s;
    }
    const QString name = path.mid(kPrefix.size());
    const DeviceType type = kitType(name.section('/', 0, 0));
    const int note = name.section('/', 1, 1).toInt();
    if (type == DeviceType::Unknown || note < 0 || note > 127) return nullptr;
    SamplePtr s = render(type, note, path);
    if (s) cache.insert(path, s);
    return s;
}

QVector<DrumPad> defaultDrumPads() {
    QVector<DrumPad> pads;
    for (int k = 0; k < drumrack::kPads; ++k) {
        const QString path = kPrefix + "808/" + QString::number(drumrack::kFirstNote + k);
        pads.push_back({builtinName(path), path, builtinSample(path)});
    }
    return pads;
}

} // namespace sessions::audio
