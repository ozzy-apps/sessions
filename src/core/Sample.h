#pragma once
// Sessions — audio samples: decoding, a shared cache and waveform overviews.
//
// A file is read whole into memory as float stereo at its own sample rate; the
// engine resamples while it plays. WAV and AIFF are decoded here. FLAC, Ogg
// Vorbis, Opus, MP3 and the rest go through libsndfile, loaded at run time when
// it's installed (it is on most Linux desktops), so building needs no extra
// package. Samples are immutable once loaded and shared between clips, devices,
// undo snapshots and the audio thread through SamplePtr.

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>

class QObject;

namespace sessions {

struct SampleData {
    QString path;                    // absolute path it was read from (empty for generated audio)
    double  rate = 48000;            // frames per second
    int     channels = 2;            // in the file; the data is always stereo (mono is duplicated)
    std::vector<float> left, right;

    // Waveform overviews: min / max per kPeakBlock frames of the mono mix, and of each channel.
    static constexpr int kPeakBlock = 256;
    std::vector<float> peakMin, peakMax, peakMinL, peakMaxL, peakMinR, peakMaxR;

    // Onsets (drum hits, plucks, syllables), in seconds from the start; always begins with 0.
    // Beats-mode warping slices the audio here (Clip::grain == 0).
    std::vector<double> transients;

    qint64 frames() const { return qint64(left.size()); }
    double seconds() const { return rate > 0 ? double(frames()) / rate : 0.0; }
    // The waveform's min / max between two times in seconds (for drawing).
    // channel: -1 the mono mix, 0 left, 1 right.
    void   range(double s0, double s1, float& lo, float& hi, int channel = -1) const;
};
using SamplePtr = std::shared_ptr<const SampleData>;

namespace samples {

// Loads a file, or returns the copy already in memory. nullptr (and why) on failure.
SamplePtr   load(const QString& path, QString* error = nullptr);
// Several files: those in memory straight away, the rest decoded side by side on worker
// threads (opening a set). In `paths` order; a failure is null, and "file: why" in errors.
QVector<SamplePtr> loadMany(const QStringList& paths, QStringList* errors = nullptr);
// In the background (drops, the browser): decodes on a worker thread, then calls `done` on
// the UI thread, unless `context` has been deleted by then.
void        loadAsync(const QStringList& paths, QObject* context, std::function<void(QVector<SamplePtr> samples, QStringList errors)> done);
SamplePtr   reversed(const SamplePtr& s);   // the same audio backwards (cached alongside the original)
// Wraps generated audio (tests, previews). `right` may be empty for mono.
SamplePtr   fromFrames(std::vector<float> left, std::vector<float> right, double rate, const QString& path = {});

bool        isAudioFile(const QString& path);   // by extension, for the formats this machine can read
QStringList nameFilters();                      // "*.wav *.aif ..." for file dialogs and folder scans
bool        sndfileAvailable();                 // libsndfile was found at run time
SamplePtr   loadWithSndfile(const QString& path, QString* error = nullptr);   // skips the built-in decoders

// WAV output (Export Audio, tests). Streams frames to disk; the header is fixed on close().
enum class WavFormat { Int16, Int24, Float32 };
class WavWriter {
public:
    WavWriter();
    ~WavWriter();
    bool open(const QString& path, int rate, WavFormat format, QString* error = nullptr);
    void write(const float* interleavedStereo, qint64 frames);   // 16 / 24 bit get TPDF dither
    bool close(QString* error = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
bool writeWav(const QString& path, const float* interleavedStereo, qint64 frames, int rate, WavFormat format, QString* error = nullptr);

} // namespace samples
} // namespace sessions
