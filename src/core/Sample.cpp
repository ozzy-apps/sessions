#include "Sample.h"
#include "audio/Builtin.h"
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>
#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <dlfcn.h>

namespace sessions {

// ---- overview ---------------------------------------------------------------

void SampleData::range(double s0, double s1, float& lo, float& hi, int channel) const {
    lo = hi = 0;
    const qint64 n = frames();
    if (n == 0 || s1 <= 0 || s0 >= seconds()) return;
    const qint64 f0 = std::clamp(qint64(std::floor(s0 * rate)), qint64(0), n - 1);
    const qint64 f1 = std::clamp(qint64(std::ceil(s1 * rate)), f0 + 1, n);
    lo = 1; hi = -1;
    if (f1 - f0 < 2 * kPeakBlock) {   // close up: the samples themselves
        for (qint64 f = f0; f < f1; ++f) {
            const float v = channel == 0 ? left[size_t(f)] : channel == 1 ? right[size_t(f)] : 0.5f * (left[size_t(f)] + right[size_t(f)]);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        return;
    }
    const std::vector<float>& mins = channel == 0 ? peakMinL : channel == 1 ? peakMinR : peakMin;
    const std::vector<float>& maxs = channel == 0 ? peakMaxL : channel == 1 ? peakMaxR : peakMax;
    const size_t b0 = size_t(f0 / kPeakBlock), b1 = std::min(mins.size(), size_t((f1 + kPeakBlock - 1) / kPeakBlock));
    for (size_t b = b0; b < b1; ++b) { lo = std::min(lo, mins[b]); hi = std::max(hi, maxs[b]); }
}

namespace samples {

namespace {

void buildPeaks(SampleData& s) {
    const size_t n = s.left.size(), blocks = (n + SampleData::kPeakBlock - 1) / SampleData::kPeakBlock;
    for (auto* v : {&s.peakMin, &s.peakMax, &s.peakMinL, &s.peakMaxL, &s.peakMinR, &s.peakMaxR}) v->assign(blocks, 0.0f);
    for (size_t b = 0; b < blocks; ++b) {
        float lo = 1, hi = -1, loL = 1, hiL = -1, loR = 1, hiR = -1;
        const size_t end = std::min(n, (b + 1) * SampleData::kPeakBlock);
        for (size_t f = b * SampleData::kPeakBlock; f < end; ++f) {
            const float l = s.left[f], r = s.right[f], v = 0.5f * (l + r);
            lo = std::min(lo, v); hi = std::max(hi, v);
            loL = std::min(loL, l); hiL = std::max(hiL, l);
            loR = std::min(loR, r); hiR = std::max(hiR, r);
        }
        s.peakMin[b] = lo; s.peakMax[b] = hi;
        s.peakMinL[b] = loL; s.peakMaxL[b] = hiL;
        s.peakMinR[b] = loR; s.peakMaxR[b] = hiR;
    }
}

// Onset detection: a high-frequency content curve (the first difference emphasizes attacks)
// over ~5 ms hops; an onset is a local peak of its rise that stands well above the rises
// around it and above the noise floor, at least 60 ms after the last one. Then it's pinned
// to the first sample of the attack, so a slice starts right where the hit does.
void findTransients(SampleData& s) {
    s.transients.assign(1, 0.0);
    s.transientStrength.assign(1, 1.0f);
    const qint64 n = s.frames();
    const int hop = std::max(32, int(std::lround(s.rate * 0.005)));
    const int hops = int(n / hop);
    if (hops < 4) return;
    std::vector<float> hfc(size_t(hops), 0.0f), rise(size_t(hops), 0.0f);
    float prev = 0, peakHfc = 0;
    for (int k = 0; k < hops; ++k) {
        float sum = 0;
        for (qint64 f = qint64(k) * hop; f < qint64(k + 1) * hop; ++f) {
            const float v = 0.5f * (s.left[size_t(f)] + s.right[size_t(f)]);
            sum += std::fabs(v - prev);
            prev = v;
        }
        hfc[size_t(k)] = sum / float(hop);
        peakHfc = std::max(peakHfc, hfc[size_t(k)]);
    }
    if (peakHfc <= 1e-6f) return;
    float peakRise = 0;
    for (int k = 1; k < hops; ++k) {
        rise[size_t(k)] = std::max(0.0f, hfc[size_t(k)] - hfc[size_t(k - 1)]);
        peakRise = std::max(peakRise, rise[size_t(k)]);
    }
    const int minGap = std::max(1, int(std::lround(0.060 * s.rate / hop)));
    int last = -minGap;
    for (int k = 1; k < hops; ++k) {
        const float r = rise[size_t(k)];
        if (r <= 0 || hfc[size_t(k)] < 0.02f * peakHfc) continue;
        bool isPeak = true;
        for (int j = std::max(1, k - 2); j <= std::min(hops - 1, k + 2); ++j) if (j != k && rise[size_t(j)] > r) isPeak = false;
        if (!isPeak) continue;
        float mean = 0;
        int count = 0;
        for (int j = std::max(1, k - 12); j <= std::min(hops - 1, k + 3); ++j) { mean += rise[size_t(j)]; ++count; }
        mean /= float(std::max(1, count));
        if (r < 1.5f * mean + 0.04f * peakRise || k - last < minGap) continue;
        last = k;
        // Pin it to the attack's first sample: where the signal first jumps by at least a
        // third of the biggest jump nearby (a few samples early, so the slice keeps it).
        const qint64 w0 = std::max<qint64>(1, qint64(k - 1) * hop), w1 = std::min<qint64>(n, qint64(k + 1) * hop);
        auto mono = [&s](qint64 f) { return 0.5f * (s.left[size_t(f)] + s.right[size_t(f)]); };
        float biggest = 0;
        for (qint64 f = w0; f < w1; ++f) biggest = std::max(biggest, std::fabs(mono(f) - mono(f - 1)));
        qint64 at = w0;
        for (qint64 f = w0; f < w1; ++f) if (std::fabs(mono(f) - mono(f - 1)) >= biggest / 3) { at = f; break; }
        const double t = double(std::max<qint64>(0, at - 4)) / s.rate;
        if (t > s.transients.back() + 0.03) {
            s.transients.push_back(t);
            s.transientStrength.push_back(peakRise > 0 ? std::clamp(r / peakRise, 0.0f, 1.0f) : 1.0f);
        }
    }
}

// Interleaved frames of `channels` -> stereo, through a per-sample converter.
template <class Read>
void deinterleave(SampleData& s, qint64 frames, int channels, Read read) {
    s.left.resize(size_t(frames));
    s.right.resize(size_t(frames));
    for (qint64 f = 0; f < frames; ++f) {
        const float l = read(f * channels);
        s.left[size_t(f)] = l;
        s.right[size_t(f)] = channels > 1 ? read(f * channels + 1) : l;
    }
}

// ---- WAV --------------------------------------------------------------------

quint16 le16(const char* p) { return qFromLittleEndian<quint16>(p); }
quint32 le32(const char* p) { return qFromLittleEndian<quint32>(p); }
quint16 be16(const char* p) { return qFromBigEndian<quint16>(p); }
quint32 be32(const char* p) { return qFromBigEndian<quint32>(p); }

float int24(const unsigned char* p, bool bigEndian) {
    int32_t v = bigEndian ? (p[0] << 16) | (p[1] << 8) | p[2] : (p[2] << 16) | (p[1] << 8) | p[0];
    if (v & 0x800000) v -= 0x1000000;
    return float(v) / 8388608.0f;
}

// Decodes PCM / float frames from `data`. `isFloat` with bits 32 or 64; ints are signed except 8-bit WAV.
bool decodeFrames(SampleData& s, const char* data, qint64 bytes, int channels, int bits, bool isFloat,
                  bool bigEndian, bool unsigned8, QString* error) {
    const int bytesPer = bits / 8;
    if (channels < 1 || bytesPer < 1 || bytesPer > 8) { if (error) *error = "Unsupported sample format."; return false; }
    const qint64 frames = bytes / (qint64(bytesPer) * channels);
    const auto* u = reinterpret_cast<const unsigned char*>(data);
    if (isFloat && bits == 32) {
        deinterleave(s, frames, channels, [&](qint64 i) {
            quint32 w = bigEndian ? be32(data + i * 4) : le32(data + i * 4);
            float v; std::memcpy(&v, &w, 4); return std::isfinite(v) ? v : 0.0f;
        });
    } else if (isFloat && bits == 64) {
        deinterleave(s, frames, channels, [&](qint64 i) {
            quint64 w = bigEndian ? qFromBigEndian<quint64>(data + i * 8) : qFromLittleEndian<quint64>(data + i * 8);
            double v; std::memcpy(&v, &w, 8); return std::isfinite(v) ? float(v) : 0.0f;
        });
    } else if (!isFloat && bits == 8) {
        deinterleave(s, frames, channels, [&](qint64 i) { return unsigned8 ? (float(u[i]) - 128.0f) / 128.0f : float(int8_t(u[i])) / 128.0f; });
    } else if (!isFloat && bits == 16) {
        deinterleave(s, frames, channels, [&](qint64 i) { return float(int16_t(bigEndian ? be16(data + i * 2) : le16(data + i * 2))) / 32768.0f; });
    } else if (!isFloat && bits == 24) {
        deinterleave(s, frames, channels, [&](qint64 i) { return int24(u + i * 3, bigEndian); });
    } else if (!isFloat && bits == 32) {
        deinterleave(s, frames, channels, [&](qint64 i) { return float(int32_t(bigEndian ? be32(data + i * 4) : le32(data + i * 4))) / 2147483648.0f; });
    } else {
        if (error) *error = QString("%1-bit %2 audio isn't supported.").arg(bits).arg(isFloat ? "float" : "integer");
        return false;
    }
    s.channels = channels;
    return true;
}

bool readWav(const QByteArray& f, SampleData& s, QString* error) {
    const char* p = f.constData();
    const qint64 n = f.size();
    if (n < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) { if (error) *error = "Not a WAV file."; return false; }
    int format = 0, channels = 0, bits = 0;
    quint32 rate = 0;
    const char* data = nullptr;
    qint64 dataBytes = 0;
    for (qint64 at = 12; at + 8 <= n;) {
        const quint32 size = le32(p + at + 4);
        const char* body = p + at + 8;
        const qint64 avail = std::min<qint64>(size, n - at - 8);
        if (!std::memcmp(p + at, "fmt ", 4) && avail >= 16) {
            format = le16(body); channels = le16(body + 2); rate = le32(body + 4); bits = le16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = le16(body + 24);   // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with it
        } else if (!std::memcmp(p + at, "data", 4)) {
            data = body; dataBytes = avail;
        }
        at += 8 + qint64(size) + (size & 1);
    }
    if (!data || !channels || !rate) { if (error) *error = "The WAV file has no audio data."; return false; }
    if (format != 1 && format != 3) { if (error) *error = QString("WAV encoding %1 isn't supported (only PCM and float).").arg(format); return false; }
    s.rate = rate;
    return decodeFrames(s, data, dataBytes, channels, bits, format == 3, false, true, error);
}

// ---- AIFF -------------------------------------------------------------------

double extended80(const unsigned char* b) {   // IEEE 754 80-bit extended, big-endian
    const int exponent = ((b[0] & 0x7F) << 8) | b[1];
    quint64 mantissa = 0;
    for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | b[2 + i];
    if (!exponent && !mantissa) return 0;
    const double v = std::ldexp(double(mantissa), exponent - 16383 - 63);
    return (b[0] & 0x80) ? -v : v;
}

bool readAiff(const QByteArray& f, SampleData& s, QString* error) {
    const char* p = f.constData();
    const qint64 n = f.size();
    if (n < 12 || std::memcmp(p, "FORM", 4) != 0 || (std::memcmp(p + 8, "AIFF", 4) != 0 && std::memcmp(p + 8, "AIFC", 4) != 0)) {
        if (error) *error = "Not an AIFF file.";
        return false;
    }
    int channels = 0, bits = 0;
    double rate = 0;
    QByteArray compression = "NONE";
    const char* data = nullptr;
    qint64 dataBytes = 0;
    for (qint64 at = 12; at + 8 <= n;) {
        const quint32 size = be32(p + at + 4);
        const char* body = p + at + 8;
        const qint64 avail = std::min<qint64>(size, n - at - 8);
        if (!std::memcmp(p + at, "COMM", 4) && avail >= 18) {
            channels = qint16(be16(body));
            bits = qint16(be16(body + 6));
            rate = extended80(reinterpret_cast<const unsigned char*>(body + 8));
            if (avail >= 22) compression = QByteArray(body + 18, 4);
        } else if (!std::memcmp(p + at, "SSND", 4) && avail >= 8) {
            const quint32 offset = be32(body);
            data = body + 8 + offset;
            dataBytes = std::max<qint64>(0, avail - 8 - offset);
        }
        at += 8 + qint64(size) + (size & 1);
    }
    if (!data || !channels || rate <= 0) { if (error) *error = "The AIFF file has no audio data."; return false; }
    s.rate = rate;
    const QByteArray c = compression.toLower();
    if (c == "none" || c == "twos") return decodeFrames(s, data, dataBytes, channels, bits, false, true, false, error);
    if (c == "sowt") return decodeFrames(s, data, dataBytes, channels, bits, false, false, false, error);
    if (c == "fl32") return decodeFrames(s, data, dataBytes, channels, 32, true, true, false, error);
    if (c == "fl64") return decodeFrames(s, data, dataBytes, channels, 64, true, true, false, error);
    if (error) *error = QString("AIFF compression '%1' isn't supported.").arg(QString::fromLatin1(compression));
    return false;
}

// ---- libsndfile, loaded at run time ---------------------------------------------

struct SfInfo { int64_t frames; int samplerate, channels, format, sections, seekable; };
struct Sndfile {
    void* (*open)(const char*, int, SfInfo*) = nullptr;
    int64_t (*readf)(void*, float*, int64_t) = nullptr;
    int (*close)(void*) = nullptr;
    const char* (*strerror)(void*) = nullptr;
    bool ok() const { return open && readf && close && strerror; }
};

const Sndfile& sndfile() {
    static const Sndfile lib = [] {
        Sndfile s;
        void* h = dlopen("libsndfile.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!h) return s;
        s.open = reinterpret_cast<void* (*)(const char*, int, SfInfo*)>(dlsym(h, "sf_open"));
        s.readf = reinterpret_cast<int64_t (*)(void*, float*, int64_t)>(dlsym(h, "sf_readf_float"));
        s.close = reinterpret_cast<int (*)(void*)>(dlsym(h, "sf_close"));
        s.strerror = reinterpret_cast<const char* (*)(void*)>(dlsym(h, "sf_strerror"));
        return s;
    }();
    return lib;
}

bool readSndfile(const QString& path, SampleData& s, QString* error) {
    const Sndfile& sf = sndfile();
    if (!sf.ok()) { if (error) *error = "This format needs libsndfile (sudo apt install libsndfile1)."; return false; }
    SfInfo info{};
    void* file = sf.open(QFile::encodeName(path).constData(), 0x10 /* SFM_READ */, &info);
    if (!file) { if (error) *error = QString::fromUtf8(sf.strerror(nullptr)); return false; }
    if (info.channels < 1 || info.samplerate <= 0) { sf.close(file); if (error) *error = "The file has no audio."; return false; }
    std::vector<float> buf(size_t(65536) * size_t(info.channels));
    for (;;) {
        const int64_t got = sf.readf(file, buf.data(), 65536);
        if (got <= 0) break;
        for (int64_t f = 0; f < got; ++f) {
            const float l = buf[size_t(f * info.channels)];
            s.left.push_back(l);
            s.right.push_back(info.channels > 1 ? buf[size_t(f * info.channels + 1)] : l);
        }
    }
    sf.close(file);
    s.rate = info.samplerate;
    s.channels = info.channels;
    if (s.left.empty()) { if (error) *error = "The file has no audio."; return false; }
    return true;
}

// What's in memory, by absolute path. An entry only counts while the file on disk is
// the one that was read (same size and modification time).
struct Cached { std::weak_ptr<const SampleData> data; qint64 size = -1; QDateTime modified; };
QHash<QString, Cached>& cache() {
    static QHash<QString, Cached> c;   // UI thread only
    return c;
}

SamplePtr finish(std::shared_ptr<SampleData> s) {
    for (float* v : {s->left.data(), s->right.data()})
        for (size_t i = 0; i < s->left.size(); ++i) if (!std::isfinite(v[i])) v[i] = 0;
    buildPeaks(*s);
    findTransients(*s);
    return s;
}

// Reads and analyses one file. Touches no shared state, so worker threads can run it.
SamplePtr decode(const QString& abs, QString* error) {
    const QFileInfo fi(abs);
    auto s = std::make_shared<SampleData>();
    s->path = abs;
    const QString ext = fi.suffix().toLower();
    QString why;
    bool ok = false;
    if (ext == "wav" || ext == "wave" || ext == "aif" || ext == "aiff" || ext == "aifc") {
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return nullptr; }
        const QByteArray bytes = f.readAll();
        ok = ext.startsWith("aif") ? readAiff(bytes, *s, &why) : readWav(bytes, *s, &why);
        if (!ok && sndfile().ok()) { *s = SampleData{}; s->path = abs; ok = readSndfile(abs, *s, &why); }   // e.g. ADPCM WAVs
    } else {
        ok = readSndfile(abs, *s, &why);
    }
    if (!ok || s->left.empty()) { if (error) *error = why.isEmpty() ? QString("The file has no audio.") : why; return nullptr; }
    return finish(s);
}

SamplePtr cached(const QFileInfo& fi) {
    const auto it = cache().constFind(fi.absoluteFilePath());
    if (it != cache().constEnd() && it->size == fi.size() && it->modified == fi.lastModified()) return it->data.lock();
    return nullptr;
}

void remember(const SamplePtr& s) {
    if (!s) return;
    const QFileInfo fi(s->path);
    cache().insert(s->path, {s, fi.size(), fi.lastModified()});
}

// Decodes paths[i] for each i in `todo` on up to one thread per core.
void decodeAll(const QStringList& abs, const std::vector<int>& todo, QVector<SamplePtr>& out, QStringList& why) {
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t k; (k = next.fetch_add(1)) < todo.size();) {
            const int i = todo[k];
            out[i] = decode(abs[i], &why[i]);
        }
    };
    const size_t threads = std::min<size_t>(todo.size(), std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
}

} // namespace

SamplePtr load(const QString& path, QString* error) {
    if (audio::isBuiltin(path)) {   // the built-in drum sounds: rendered, not read
        SamplePtr s = audio::builtinSample(path);
        if (!s && error) *error = "There's no built-in sound called " + path + ".";
        return s;
    }
    const QFileInfo fi(path);
    if (!fi.exists()) { if (error) *error = "The file doesn't exist."; return nullptr; }
    if (SamplePtr hit = cached(fi)) return hit;
    SamplePtr out = decode(fi.absoluteFilePath(), error);
    remember(out);
    return out;
}

QVector<SamplePtr> loadMany(const QStringList& paths, QStringList* errors) {
    QVector<SamplePtr> out(paths.size());
    QStringList abs, why;
    std::vector<int> todo;
    for (int i = 0; i < paths.size(); ++i) {
        const QFileInfo fi(paths[i]);
        abs << fi.absoluteFilePath();
        why << QString();
        if (audio::isBuiltin(paths[i])) {
            if (!(out[i] = audio::builtinSample(paths[i]))) why[i] = "There's no such built-in sound.";
        } else if (!fi.exists()) why[i] = "The file doesn't exist.";
        else if (!(out[i] = cached(fi)) && !abs.mid(0, i).contains(abs[i])) todo.push_back(i);
    }
    decodeAll(abs, todo, out, why);
    for (int i = 0; i < paths.size(); ++i) {
        if (audio::isBuiltin(paths[i])) {
            if (!out[i] && errors) *errors << paths[i] + ": " + why[i];
            continue;
        }
        if (!out[i] && why[i].isEmpty()) {   // the same file twice
            const int first = int(abs.indexOf(abs[i]));
            out[i] = out[first];
            why[i] = why[first];
        }
        remember(out[i]);
        if (!out[i] && errors) *errors << QFileInfo(paths[i]).fileName() + ": " + (why[i].isEmpty() ? QString("The file has no audio.") : why[i]);
    }
    return out;
}

void loadAsync(const QStringList& paths, QObject* context, std::function<void(QVector<SamplePtr>, QStringList)> done) {
    // what's in memory is taken here; only the rest goes to the worker
    QVector<SamplePtr> out(paths.size());
    QStringList abs, why;
    std::vector<int> todo;
    for (int i = 0; i < paths.size(); ++i) {
        const QFileInfo fi(paths[i]);
        abs << fi.absoluteFilePath();
        why << QString();
        if (audio::isBuiltin(paths[i])) {
            if (!(out[i] = audio::builtinSample(paths[i]))) why[i] = "There's no such built-in sound.";
        } else if (!fi.exists()) why[i] = "The file doesn't exist.";
        else if (!(out[i] = cached(fi))) todo.push_back(i);
    }
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([paths, abs, todo, out, why, guard, done = std::move(done)]() mutable {
        decodeAll(abs, todo, out, why);
        // back on the UI thread: the cache and the callback live there
        QMetaObject::invokeMethod(QCoreApplication::instance(), [paths, todo, out, why, guard, done]() mutable {
            if (!guard) return;
            for (int i : todo) {   // one copy in memory: if the file was loaded meanwhile, share that
                if (!out[i]) continue;
                if (SamplePtr hit = cached(QFileInfo(paths[i]))) out[i] = hit; else remember(out[i]);
            }
            QStringList errors;
            for (int i = 0; i < paths.size(); ++i)
                if (!out[i]) errors << QFileInfo(paths[i]).fileName() + ": " + (why[i].isEmpty() ? QString("The file has no audio.") : why[i]);
            done(out, errors);
        }, Qt::QueuedConnection);
    });
}

SamplePtr loadWithSndfile(const QString& path, QString* error) {
    auto s = std::make_shared<SampleData>();
    s->path = QFileInfo(path).absoluteFilePath();
    if (!readSndfile(s->path, *s, error)) return nullptr;
    return finish(s);
}

SamplePtr reversed(const SamplePtr& src) {
    if (!src) return nullptr;
    const QString key = src->path.isEmpty() ? QString() : src->path + QStringLiteral("\n(reversed)");
    if (!key.isEmpty()) if (SamplePtr hit = cache().value(key).data.lock()) if (hit->left.size() == src->left.size()) return hit;
    auto s = std::make_shared<SampleData>(*src);
    std::reverse(s->left.begin(), s->left.end());
    std::reverse(s->right.begin(), s->right.end());
    SamplePtr out = finish(s);
    if (!key.isEmpty()) cache().insert(key, {out, -1, {}});
    return out;
}

SamplePtr fromFrames(std::vector<float> left, std::vector<float> right, double rate, const QString& path) {
    auto s = std::make_shared<SampleData>();
    s->channels = right.empty() ? 1 : 2;
    if (right.empty()) right = left;
    right.resize(left.size());
    s->left = std::move(left);
    s->right = std::move(right);
    s->rate = rate;
    s->path = path;
    return finish(s);
}

bool sndfileAvailable() { return sndfile().ok(); }

QStringList nameFilters() {
    QStringList f{"*.wav", "*.wave", "*.aif", "*.aiff", "*.aifc"};
    if (sndfileAvailable()) f << "*.flac" << "*.ogg" << "*.oga" << "*.opus" << "*.mp3";
    return f;
}

bool isAudioFile(const QString& path) {
    const QString ext = "*." + QFileInfo(path).suffix().toLower();
    return nameFilters().contains(ext);
}

// ---- WAV output ---------------------------------------------------------------

struct WavWriter::Impl {
    QFile file;
    WavFormat format = WavFormat::Int16;
    int rate = 48000;
    qint64 frames = 0;
    std::mt19937 rng{12345};
    std::uniform_real_distribution<float> uni{-0.5f, 0.5f};
    QByteArray buf;
};

WavWriter::WavWriter() : d(new Impl) {}
WavWriter::~WavWriter() { if (d->file.isOpen()) close(); }

bool WavWriter::open(const QString& path, int rate, WavFormat format, QString* error) {
    d->file.setFileName(path);
    d->format = format;
    d->rate = rate;
    d->frames = 0;
    if (!d->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (error) *error = d->file.errorString(); return false; }
    d->file.write(QByteArray(44, '\0'));   // header, filled in by close()
    return true;
}

void WavWriter::write(const float* in, qint64 frames) {
    const int bytes = d->format == WavFormat::Int16 ? 2 : d->format == WavFormat::Int24 ? 3 : 4;
    d->buf.resize(int(frames * 2 * bytes));
    char* out = d->buf.data();
    for (qint64 i = 0; i < frames * 2; ++i) {
        const float x = std::clamp(in[i], -1.0f, 1.0f);
        if (d->format == WavFormat::Float32) {
            qToLittleEndian<float>(x, out + i * 4);
        } else if (d->format == WavFormat::Int16) {
            const float dither = d->uni(d->rng) + d->uni(d->rng);   // TPDF, one LSB peak to peak each
            const auto v = int16_t(std::clamp(std::lround(x * 32767.0f + dither), -32768L, 32767L));
            qToLittleEndian<int16_t>(v, out + i * 2);
        } else {
            const float dither = d->uni(d->rng) + d->uni(d->rng);
            const auto v = int32_t(std::clamp(std::lround(double(x) * 8388607.0 + dither), -8388608L, 8388607L));
            out[i * 3] = char(v & 0xFF); out[i * 3 + 1] = char((v >> 8) & 0xFF); out[i * 3 + 2] = char((v >> 16) & 0xFF);
        }
    }
    d->file.write(d->buf);
    d->frames += frames;
}

bool WavWriter::close(QString* error) {
    if (!d->file.isOpen()) return false;
    const quint16 bits = d->format == WavFormat::Int16 ? 16 : d->format == WavFormat::Int24 ? 24 : 32;
    const quint16 tag = d->format == WavFormat::Float32 ? 3 : 1;
    const quint32 block = 2 * bits / 8, dataBytes = quint32(d->frames * block);
    QByteArray h(44, '\0');
    char* p = h.data();
    std::memcpy(p, "RIFF", 4); qToLittleEndian<quint32>(36 + dataBytes, p + 4); std::memcpy(p + 8, "WAVE", 4);
    std::memcpy(p + 12, "fmt ", 4); qToLittleEndian<quint32>(16, p + 16); qToLittleEndian<quint16>(tag, p + 20);
    qToLittleEndian<quint16>(2, p + 22); qToLittleEndian<quint32>(quint32(d->rate), p + 24);
    qToLittleEndian<quint32>(quint32(d->rate) * block, p + 28); qToLittleEndian<quint16>(quint16(block), p + 32);
    qToLittleEndian<quint16>(bits, p + 34);
    std::memcpy(p + 36, "data", 4); qToLittleEndian<quint32>(dataBytes, p + 40);
    d->file.seek(0);
    d->file.write(h);
    const bool ok = d->file.error() == QFileDevice::NoError;
    if (!ok && error) *error = d->file.errorString();
    d->file.close();
    return ok;
}

bool writeWav(const QString& path, const float* in, qint64 frames, int rate, WavFormat format, QString* error) {
    WavWriter w;
    if (!w.open(path, rate, format, error)) return false;
    w.write(in, frames);
    return w.close(error);
}

} // namespace samples
} // namespace sessions
