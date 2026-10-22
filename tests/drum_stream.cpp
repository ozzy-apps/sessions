// Long recordings of a recorded drum library stream from disk: only their first half second stays
// in memory, the rest is read ahead of playback by the streamer thread. A streamed kit sounds
// exactly like the same kit held whole, offline (which waits for a slow disk) and in real time;
// a stream that falls behind fades out instead of clicking or stalling; slots are shared and given
// back; a missing file fails quickly; Preferences can turn streaming off (the kit re-reads, playing
// as before meanwhile); the editor draws the whole recording. A generated library (real WAVs and a
// library.json in the shipped format; WAV and FLAC) stands in for the downloads; settings are temporary.
#include "audio/Devices.h"
#include "audio/DrumSamples.h"
#include "audio/DrumStream.h"
#include "audio/Engine.h"
#include "audio/MidiInput.h"
#include "core/DeviceSpecs.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/DeviceView.h"
#include "ui/Preferences.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QLibrary>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <thread>

using namespace sessions;
namespace ds = sessions::drumstudio;
namespace drums = sessions::audio::drums;
using Loader = drums::ContentLoader;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
    else std::printf("ok:   %s\n", text);
    std::fflush(stdout);
}
thread_local bool watch = false;   // count allocations while rendering only
std::atomic<int> allocations{0};
constexpr int kRate = 48000;

// A 16-bit WAV, `channels` wide, `seconds` long; sample(channel, frame) gives each value (-1..1).
void writeWav(const QString& path, int channels, double seconds, const std::function<double(int, int)>& sample) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path); f.open(QIODevice::WriteOnly);
    const int n = int(seconds * kRate);
    QByteArray data(n * 2 * channels, 0);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c)
            qToLittleEndian<qint16>(qint16(std::lround(std::clamp(sample(c, i), -1.0, 1.0) * 32767)), data.data() + (i * channels + c) * 2);
    auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); f.write(b, 4); };
    auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); f.write(b, 2); };
    f.write("RIFF"); u32(36 + quint32(data.size())); f.write("WAVEfmt "); u32(16); u16(1); u16(quint16(channels)); u32(kRate);
    u32(quint32(kRate * 2 * channels)); u16(quint16(2 * channels)); u16(16);
    f.write("data"); u32(quint32(data.size())); f.write(data);
}
// The same, as 24-bit FLAC (the format of most of the shipped libraries), written by libsndfile.
bool writeFlac(const QString& path, int channels, double seconds, const std::function<double(int, int)>& sample) {
    QDir().mkpath(QFileInfo(path).path());
    static QLibrary lib("libsndfile.so.1");
    using Open = void* (*)(const char*, int, drums::SndInfo*);
    using Write = int64_t (*)(void*, const float*, int64_t);
    using Close = int (*)(void*);
    auto open = reinterpret_cast<Open>(lib.resolve("sf_open"));
    auto write = reinterpret_cast<Write>(lib.resolve("sf_writef_float"));
    auto close = reinterpret_cast<Close>(lib.resolve("sf_close"));
    if (!open || !write || !close) return false;
    drums::SndInfo info{}; info.samplerate = kRate; info.channels = channels; info.format = 0x170000 | 0x0003;   // FLAC, 24-bit
    void* f = open(QFile::encodeName(path).constData(), 0x20, &info);
    if (!f) return false;
    const int n = int(seconds * kRate);
    std::vector<float> data(size_t(n) * size_t(channels));
    for (int i = 0; i < n; ++i) for (int c = 0; c < channels; ++c) data[size_t(i * channels + c)] = float(sample(c, i));
    const bool ok = write(f, data.data(), n) == n;
    close(f);
    return ok;
}
// A decaying tone with a slow wobble, so every stretch of it differs.
double ring(double hz, int i, double seconds, double phase = 0) {
    const double t = double(i) / kRate;
    return 0.5 * std::sin(2 * M_PI * hz * t + phase + 0.3 * std::sin(2 * M_PI * 3 * t)) * std::exp(-t * 1.2) * (1.0 - t / seconds);
}
std::vector<float> renderProc(audio::DeviceProcessor& proc, std::vector<float>& params, const drums::Content* content, int frames,
                              const std::vector<std::pair<int, int>>& notes, bool wait, bool paced) {
    std::vector<float> out(size_t(frames) * 2);
    audio::Context ctx; ctx.drumContent = content; ctx.waitForStreams = wait;
    float l[audio::kMaxBlock], r[audio::kMaxBlock];
    const auto start = std::chrono::steady_clock::now();
    for (int at = 0; at < frames; at += audio::kMaxBlock) {
        const int n = std::min(audio::kMaxBlock, frames - at);
        audio::EventList ev;
        for (const auto& [frame, pitch] : notes) if (frame >= at && frame < at + n) ev.add({frame - at, pitch, 0.85f, 48000});
        std::fill_n(l, n, 0.f); std::fill_n(r, n, 0.f);
        watch = true; proc.render(ev, l, r, n, params.data(), ctx); watch = false;
        for (int i = 0; i < n; ++i) { out[size_t(at + i) * 2] = l[i]; out[size_t(at + i) * 2 + 1] = r[i]; }
        if (paced) std::this_thread::sleep_until(start + std::chrono::microseconds(qint64(double(at + n) / kRate * 1e6)));
    }
    return out;
}
double maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = a.size() == b.size() ? 0 : 1e9;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) m = std::max(m, double(std::abs(a[i] - b[i])));
    return m;
}
double energy(const std::vector<float>& x, int from, int to) {
    double e = 0;
    for (int i = from; i < to && size_t(i) * 2 + 1 < x.size(); ++i) e += double(x[size_t(i) * 2]) * x[size_t(i) * 2] + double(x[size_t(i) * 2 + 1]) * x[size_t(i) * 2 + 1];
    return e;
}
double biggestStep(const std::vector<float>& x, int from, int to) {
    double m = 0;
    for (int i = std::max(1, from); i < to && size_t(i) * 2 + 1 < x.size(); ++i)
        m = std::max({m, double(std::abs(x[size_t(i) * 2] - x[size_t(i - 1) * 2])), double(std::abs(x[size_t(i) * 2 + 1] - x[size_t(i - 1) * 2 + 1]))});
    return m;
}
bool finite(const std::vector<float>& x) { for (float v : x) if (!std::isfinite(v)) return false; return true; }
double libraryPos(int bank) { return choicePosition(bank + 1, 7); }   // Kit recipe, then the six
void settle(audio::Engine& e) {
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QTest::qWait(5); }
    e.flushPublish();
    std::vector<float> scratch(512);
    e.process(scratch.data(), 256);
    e.collectGarbage();
}
}

void* operator new(std::size_t n) { if (watch) ++allocations; if (void* p = std::malloc(n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const QString shots = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QTemporaryDir temporary, content;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    qputenv("SESSIONS_DRUM_CONTENT_ROOT", content.path().toUtf8());
    // The generated library. Snare (38): a 3 s stereo close mic, overheads as a pair of mono files
    // (2.6 s and 3 s: different lengths), a 2.4 s mono room. Kick (36): 0.4 s, short enough to stay
    // whole. Crash (49): a 3 s hit whose overhead pair is one long and one short file (held whole).
    const QString root = content.path();
    writeWav(root + "/audio/snare_close.wav", 2, 3.0, [](int c, int i) { return ring(c ? 310 : 300, i, 3.0); });
    writeWav(root + "/audio/snare_oh_l.wav", 1, 2.6, [](int, int i) { return ring(520, i, 2.6); });
    writeWav(root + "/audio/snare_oh_r.wav", 1, 3.0, [](int, int i) { return ring(530, i, 3.0, 1.0); });
    writeWav(root + "/audio/snare_room.wav", 1, 2.4, [](int, int i) { return ring(150, i, 2.4); });
    writeWav(root + "/audio/kick.wav", 1, 0.4, [](int, int i) { return ring(60, i, 0.4); });
    writeWav(root + "/audio/crash.wav", 1, 3.0, [](int, int i) { return ring(900, i, 3.0); });
    writeWav(root + "/audio/crash_oh_l.wav", 1, 3.0, [](int, int i) { return ring(1100, i, 3.0); });
    writeWav(root + "/audio/crash_oh_r.wav", 1, 0.5, [](int, int i) { return ring(1150, i, 0.5); });
    const bool flac = writeFlac(root + "/audio/ride.flac", 2, 4.0, [](int c, int i) { return ring(c ? 760 : 740, i, 4.0, 0.5); });
    auto note = [](const QJsonObject& hit) { return QJsonArray{QJsonObject{{"low", 1}, {"high", 127}, {"hits", QJsonArray{hit}}}}; };
    QJsonObject notes;
    notes.insert("38", note(QJsonObject{{"close", "audio/snare_close.wav"}, {"overhead", QJsonArray{"audio/snare_oh_l.wav", "audio/snare_oh_r.wav"}}, {"room", "audio/snare_room.wav"}}));
    notes.insert("36", note(QJsonObject{{"close", "audio/kick.wav"}}));
    notes.insert("51", note(QJsonObject{{"close", "audio/ride.flac"}}));
    notes.insert("49", note(QJsonObject{{"close", "audio/crash.wav"}, {"overhead", QJsonArray{"audio/crash_oh_l.wav", "audio/crash_oh_r.wav"}}}));
    const QJsonArray banks{QJsonObject{{"id", "Long"}, {"name", "Long"}, {"notes", notes}}};
    { QFile f(root + "/library.json"); f.open(QIODevice::WriteOnly); f.write(QJsonDocument(QJsonObject{{"version", 1}, {"banks", banks}}).toJson()); }
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    auto* loader = Loader::instance();
    loader->setBudget(0);
    QSettings settings("Sessions", "Sessions");

    // ---- decoding: the head in memory, the rest left on disk
    check(drums::streamingEnabled(), "setting: streaming is on to begin with");
    settings.setValue("drums/stream", false);
    const auto whole = drums::loadContentBank(0);
    settings.setValue("drums/stream", true);
    loader->reload();   // nobody wants it: let go, so the next read decodes again
    const auto streamed = drums::loadContentBank(0);
    check(whole && streamed && whole != streamed, "setup: the library decodes, whole and streamed");
    if (!whole || !streamed) { std::printf("drum_stream: %d checks, %d failure(s)\n", checks, failures + 1); return 1; }
    const auto* wholeSnare = drums::recordedHit(*whole, 38, 100, 0);
    const auto* snare = drums::recordedHit(*streamed, 38, 100, 0);
    const auto* kick = drums::recordedHit(*streamed, 36, 100, 0);
    const auto* crash = drums::recordedHit(*streamed, 49, 100, 0);
    check(whole->streamed == 0 && whole->frames == size_t(whole->wholeFrames), "decode: with streaming off every recording is held whole");
    check(snare && snare->close.stream >= 0 && snare->close.mono.size() == size_t(kRate / 2) && snare->close.right.size() == size_t(kRate / 2)
          && snare->close.length() == 3 * kRate, "decode: a long stereo recording keeps its first half second; its length is the whole file's");
    check(snare && snare->overhead.stream >= 0 && snare->overhead.right.size() == snare->overhead.mono.size() && snare->overhead.length() == 3 * kRate,
          "decode: a pair of mono files streams together, as long as the longer one");
    check(snare && snare->room.stream >= 0 && snare->room.right.empty() && snare->room.length() == qint64(2.4 * kRate), "decode: a mono recording streams");
    check(kick && kick->close.stream < 0 && kick->close.mono.size() == size_t(0.4 * kRate), "decode: a short recording stays whole");
    check(crash && crash->close.stream >= 0 && crash->overhead.stream < 0 && crash->overhead.mono.size() == size_t(3 * kRate),
          "decode: a pair with one short file is held whole (both channels)");
    const auto* ride = drums::recordedHit(*streamed, 51, 100, 0);
    const auto* wholeRide = drums::recordedHit(*whole, 51, 100, 0);
    check(flac && ride && wholeRide && ride->close.stream >= 0 && ride->close.length() == 4 * kRate && ride->close.right.size() == size_t(kRate / 2),
          "decode: a long FLAC recording streams too");
    check(ride && wholeRide && drums::wholeRecording(ride->close)->mono == wholeRide->close.mono, "decode: a FLAC recording read whole matches the whole decode");
    check(streamed->streamed == 5 && streamed->wholeFrames == whole->wholeFrames && streamed->frames * 5 < whole->frames * 2,
          "decode: streaming holds a fraction of the library in memory");
    {
        const auto drawn = drums::wholeRecording(snare->close);
        const auto drawnOh = drums::wholeRecording(snare->overhead);
        check(drawn->mono == wholeSnare->close.mono && drawn->right == wholeSnare->close.right && drawnOh->mono == wholeSnare->overhead.mono
              && drawnOh->right == wholeSnare->overhead.right, "display: a streamed recording read whole matches the whole decode sample for sample");
    }

    // ---- a streamed kit sounds exactly like the whole one
    std::vector<float> params(size_t(ds::kParams));
    { const DeviceSpec* s = specOf(DeviceType::DrumStudio); for (int i = 0; i < ds::kParams; ++i) params[size_t(i)] = float(s->params[size_t(i)].def); }
    params[ds::ContentLibrary] = float(libraryPos(0));
    auto wholeContent = std::make_shared<drums::Content>(); wholeContent->banks[0] = whole;
    auto streamContent = std::make_shared<drums::Content>(); streamContent->banks[0] = streamed;
    const std::vector<std::pair<int, int>> pattern{{0, 38}, {9000, 36}, {20000, 51}, {30000, 49}, {60000, 38}, {61000, 38}, {90000, 51}};
    const int length = int(4.4 * kRate);
    auto proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
    const auto reference = renderProc(*proc, params, wholeContent.get(), length, pattern, false, false);
    check(energy(reference, 0, length) > 1 && energy(reference, int(1.5 * kRate), length) > 1e-2, "setup: the whole kit rings for seconds");
    proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
    const auto offline = renderProc(*proc, params, streamContent.get(), length, pattern, true, false);
    check(maxDiff(offline, reference) == 0, "offline: rendering faster than real time, the streamed kit is identical to the whole one");
    drums::setStreamDelayForTests(8);   // a slow disk: about 80 ms behind real time for each stream
    proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
    QElapsedTimer timer; timer.start();
    const auto slowDisk = renderProc(*proc, params, streamContent.get(), length, pattern, true, false);
    check(maxDiff(slowDisk, reference) == 0, "offline: with a slow disk it waits, and is still identical");
    drums::setStreamDelayForTests(0);
    const qint64 underrunsBefore = drums::streamUnderruns();
    proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
    const auto live = renderProc(*proc, params, streamContent.get(), length, pattern, false, true);
    check(maxDiff(live, reference) == 0 && drums::streamUnderruns() == underrunsBefore, "real time: the streamer keeps ahead; identical, with no underruns");
    proc.reset();
    check(drums::streams::inUse() == 0, "slots: every voice gave its slot back");

    // ---- a stream that falls behind fades out, in real time, without a click
    {
        proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
        const auto single = renderProc(*proc, params, wholeContent.get(), int(1.6 * kRate), {{0, 38}}, false, false);
        drums::setStreamDelayForTests(600);   // far slower than playback
        proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
        const auto starved = renderProc(*proc, params, streamContent.get(), int(1.6 * kRate), {{0, 38}}, false, true);
        drums::setStreamDelayForTests(0);
        const int head = kRate / 2;
        check(drums::streamUnderruns() > underrunsBefore, "underrun: counted");
        const int played = head * 4 / 5;   // safely inside the head, whatever the drum's tuning
        check(finite(starved) && maxDiff(std::vector<float>(starved.begin(), starved.begin() + played * 2), std::vector<float>(single.begin(), single.begin() + played * 2)) == 0,
              "underrun: the head plays as recorded");
        check(energy(starved, head + kRate / 4, int(1.6 * kRate)) < energy(single, head + kRate / 4, int(1.6 * kRate)) * 1e-3,
              "underrun: then the hit fades out within milliseconds rather than playing garbage");
        check(biggestStep(starved, played, head + 4000) <= biggestStep(single, played, head + 4000) * 1.5 + 1e-4, "underrun: no click where it ran out");
        proc.reset();
        check(drums::streams::inUse() == 0, "underrun: the slots are given back");
    }

    // ---- a file that has gone fails at once instead of stalling an export
    {
        QFile::rename(root + "/audio/snare_close.wav", root + "/audio/snare_close.moved");
        proc = audio::createProcessor(DeviceType::DrumStudio, kRate);
        timer.restart();
        const auto gone = renderProc(*proc, params, streamContent.get(), kRate, {{0, 38}}, true, false);
        const qint64 ms = timer.elapsed();
        QFile::rename(root + "/audio/snare_close.moved", root + "/audio/snare_close.wav");
        check(ms < 1500 && finite(gone), "missing file: an offline render doesn't wait for it");
        check(energy(gone, 0, kRate / 2) > 1e-2 && biggestStep(gone, kRate / 2 - 500, kRate / 2 + 2000) < 0.2, "missing file: the head plays, then the hit fades");
        proc.reset();
    }

    // ---- slots are shared, limited and given back
    {
        std::vector<std::shared_ptr<audio::DeviceProcessor>> kits;
        audio::Context ctx; ctx.drumContent = streamContent.get();
        float l[audio::kMaxBlock], r[audio::kMaxBlock];
        for (int k = 0; k < 6; ++k) kits.push_back(audio::createProcessor(DeviceType::DrumStudio, kRate));
        for (int round = 0; round < 70; ++round)
            for (auto& kit : kits) {
                audio::EventList ev; ev.add({0, 38, 0.85f, 48000});
                std::fill_n(l, audio::kMaxBlock, 0.f); std::fill_n(r, audio::kMaxBlock, 0.f);
                watch = true; kit->render(ev, l, r, 64, params.data(), ctx); watch = false;
            }
        check(drums::streams::inUse() == drums::kStreamSlots, "slots: six kits playing 64 long hits each share the pool, up to its size");
        audio::EventList none;
        bool ok = true;
        for (int b = 0; b < 80; ++b) for (auto& kit : kits) { std::fill_n(l, audio::kMaxBlock, 0.f); std::fill_n(r, audio::kMaxBlock, 0.f); watch = true; kit->render(none, l, r, audio::kMaxBlock, params.data(), ctx); watch = false; for (int i = 0; i < audio::kMaxBlock; ++i) ok &= std::isfinite(l[i]) && std::isfinite(r[i]); }
        check(ok, "slots: hits that found none play their head and fade");
        kits[0]->panic();
        check(drums::streams::inUse() < drums::kStreamSlots, "slots: stopping a kit gives its slots back");
        kits.clear();
        check(drums::streams::inUse() == 0, "slots: removing kits gives the rest back");
    }

    // ---- the engine: exports wait for the disk; Preferences turns streaming off and on
    {
        Project p;
        p.scenes = {"1"};
        p.addTrack("Drums", QColor("#c08a5a"));
        p.tracks[0].devices = {p.makeDevice("Drum Studio", DeviceKind::Instrument)};
        const int deviceId = p.tracks[0].devices[0].id;
        p.tracks[0].devices[0].params[ds::ContentLibrary].value = libraryPos(0);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        engine.setBackgroundContent(true);
        engine.publish();
        loader->waitIdle(10000);
        settle(engine);
        auto playSnare = [&] {
            std::vector<float> out(size_t(2 * kRate) * 2);
            std::vector<float> rest(size_t(kRate) * 2);
            engine.process(rest.data(), kRate);   // the previous hit rings out
            transport.previewNote(0, 38, 0.85);
            for (int at = 0; at < 2 * kRate; at += 256) { watch = true; engine.process(out.data() + size_t(at) * 2, 256); watch = false; }
            return out;
        };
        const auto live = loader->ready(0);
        check(live && live->streamed == 5, "engine: the live kit streams");
        drums::setStreamDelayForTests(4);
        const auto a = playSnare();
        drums::setStreamDelayForTests(0);
        check(energy(a, int(0.6 * kRate), 2 * kRate) > 1e-2, "engine: without a sound card it waits for the disk, so the tail plays in full");

        audio::MidiInput midi;
        PreferencesDialog prefs(&engine, &midi, {});
        auto* toggle = prefs.findChild<QCheckBox*>("drumStream");
        check(toggle && toggle->isChecked() && toggle->text() == "Stream long recordings from disk", "preferences: the switch, on");
        if (!shots.isEmpty()) {
            prefs.resize(900, 640); prefs.show();
            QApplication::processEvents();
            if (toggle) for (QWidget* w = toggle; w; w = w->parentWidget())   // the page that holds it
                if (auto* scroll = qobject_cast<QScrollArea*>(w)) { scroll->ensureWidgetVisible(toggle); break; }
            QApplication::processEvents();
            QDir().mkpath(shots); prefs.grab().save(shots + "/drum-stream-preferences.png");
        }
        loader->setReadDelayForTests(40);
        if (toggle) toggle->setChecked(false);
        check(!settings.value("drums/stream", true).toBool(), "preferences: turning it off is saved");
        check(loader->ready(0) == live && loader->status(0).state == Loader::State::Ready, "preferences: meanwhile the kit plays as before");
        loader->waitIdle(10000);
        settle(engine);
        const auto reread = loader->ready(0);
        check(reread && reread != live && reread->streamed == 0 && loader->memoryWhole() == loader->memoryUsed(), "preferences: the kit is read again, whole");
        const auto b = playSnare();
        double peak = 0; for (float v : a) peak = std::max(peak, double(std::abs(v)));
        check(peak > 1e-2 && maxDiff(a, b) < peak * 1e-3, "preferences: and sounds the same");
        if (toggle) toggle->setChecked(true);
        loader->waitIdle(10000);
        settle(engine);
        check(loader->ready(0) && loader->ready(0)->streamed == 5 && loader->memoryUsed() * 5 < loader->memoryWhole() * 2, "preferences: turned on again, it streams");
        loader->setReadDelayForTests(0);

        // The editor's waveform reads the whole recording of a streamed hit.
        DeviceView view(&p, &transport);
        view.resize(1480, 780); view.show(); view.rebuild(); QApplication::processEvents();
        view.expandDevice(deviceId); QApplication::processEvents();
        QTest::qWait(60);
        if (!shots.isEmpty()) { QDir().mkpath(shots); view.grab().save(shots + "/drum-stream-editor.png"); }
        check(view.isVisible(), "editor: a streamed kit's editor opens");
    }

    check(allocations == 0, "rendering streamed hits allocates nothing");
    std::printf("drum_stream: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
