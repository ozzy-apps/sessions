// Recorded drum libraries load in the background: the window never waits, each drum plays its
// previous kit (or the built-in one) until the new one is ready, a kit that's switched away from
// mid-load is cancelled, unused kits are kept within the memory budget, automation preloads only
// the kits it reaches, and Drum Studio says what's loading. A generated library (real WAVs and a
// library.json in the shipped format) stands in for the six downloads; settings are temporary.
#include "audio/Devices.h"
#include "audio/DrumSamples.h"
#include "audio/Engine.h"
#include "core/DeviceSpecs.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace sessions;
namespace ds = sessions::drumstudio;
using Loader = audio::drums::ContentLoader;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", text); }
    else std::printf("ok:   %s\n", text);
    std::fflush(stdout);
}
thread_local bool watch = false;   // count allocations on the audio callback's thread only
std::atomic<int> allocations{0};

// A 16-bit mono WAV of a sine at `hz` (amplitude 0.5, fading over `seconds`), or garbage.
void writeWav(const QString& path, double hz, double seconds = 0.4, bool garbage = false) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path); f.open(QIODevice::WriteOnly);
    if (garbage) { f.write("this is not a recording"); return; }
    const int rate = 48000, n = int(seconds * rate);
    QByteArray data(n * 2, 0);
    for (int i = 0; i < n; ++i) {
        const double v = 0.5 * std::sin(2 * M_PI * hz * i / rate) * (1.0 - double(i) / n);
        qToLittleEndian<qint16>(qint16(std::lround(v * 32767)), data.data() + i * 2);
    }
    auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); f.write(b, 4); };
    auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); f.write(b, 2); };
    f.write("RIFF"); u32(36 + quint32(data.size())); f.write("WAVEfmt "); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data"); u32(quint32(data.size())); f.write(data);
}
// A bank: kick, snare and closed hat, `hits` round robins each, every hit the same tone.
QJsonObject bank(const QString& root, const QString& name, double hz, int hits, int broken = 0) {
    QJsonObject notes;
    for (int note : {36, 38, 42}) {
        QJsonArray list;
        for (int h = 0; h < hits; ++h) {
            const QString rel = QString("audio/%1/n%2_rr%3.wav").arg(name).arg(note).arg(h);
            if (broken == 1) { if (h == 0 && note == 36) writeWav(root + "/" + rel, hz, 0.4, true); }   // installed, but unreadable
            else if (broken != 2) writeWav(root + "/" + rel, hz);                                         // 2: not there at all
            list.append(QJsonObject{{"close", rel}});
        }
        notes.insert(QString::number(note), QJsonArray{QJsonObject{{"low", 1}, {"high", 127}, {"hits", list}}});
    }
    return QJsonObject{{"id", name}, {"name", name}, {"notes", notes}};
}
// Energy of `x` (left channel) at `hz`: which library a hit came from.
double tone(const std::vector<float>& x, double hz, double rate) {
    double re = 0, im = 0;
    for (size_t i = 0; i < x.size() / 2; ++i) { const double a = 2 * M_PI * hz * double(i) / rate; re += x[i * 2] * std::cos(a); im += x[i * 2] * std::sin(a); }
    return std::sqrt(re * re + im * im) / double(x.size() / 2);
}
std::vector<float> render(audio::Engine& e, int frames) {
    std::vector<float> out(size_t(frames) * 2);
    for (int done = 0; done < frames;) {
        const int n = std::min(256, frames - done);
        watch = true; e.process(out.data() + size_t(done) * 2, n); watch = false;
        done += n;
    }
    return out;
}
// One snare hit through the engine, after the previous one has rung out.
std::vector<float> hit(audio::Engine& e, Transport& t) {
    render(e, 48000);
    t.previewNote(0, 38, 0.85);
    return render(e, 14400);
}
enum class Heard { A, B, Neither };
Heard heard(const std::vector<float>& x, double rate) {
    const double a = tone(x, 300, rate), b = tone(x, 1200, rate);
    if (a > 4 * b && a > 1e-4) return Heard::A;
    if (b > 4 * a && b > 1e-4) return Heard::B;
    return Heard::Neither;
}
double libraryPos(int bank) { return choicePosition(bank + 1, 7); }   // Kit recipe, then the six
void settle(audio::Engine& e) {   // the loader's signals arrive, the engine republishes and the audio thread adopts it
    for (int i = 0; i < 5; ++i) { QApplication::processEvents(); QTest::qWait(5); }
    e.flushPublish();
    render(e, 256);
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
    // The generated library: A (300 Hz) and B (1200 Hz) load; C is installed but unreadable;
    // D isn't installed; the last two slots are empty.
    const QString root = content.path();
    const QJsonArray banks{bank(root, "Alpha", 300, 20), bank(root, "Beta", 1200, 20), bank(root, "Broken", 600, 4, 1), bank(root, "Absent", 800, 4, 2)};
    { QFile f(root + "/library.json"); f.open(QIODevice::WriteOnly); f.write(QJsonDocument(QJsonObject{{"version", 1}, {"banks", banks}}).toJson()); }
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    auto* loader = Loader::instance();
    loader->setBudget(0);

    // ---- which kits a Drum Studio can reach
    {
        const auto defaults = [] { std::vector<float> p(size_t(ds::kParams)); const DeviceSpec* s = specOf(DeviceType::DrumStudio); for (int i = 0; i < ds::kParams; ++i) p[size_t(i)] = float(s->params[size_t(i)].def); return p; };
        auto p = defaults();
        p[ds::ContentLibrary] = float(libraryPos(0));
        auto reach = audio::drums::reachableBanks(p.data(), {});
        check(reach[0] && !reach[1] && !reach[2] && !reach[3] && !reach[4] && !reach[5], "reach: the selected library, nothing else");
        reach = audio::drums::reachableBanks(p.data(), {{ds::ContentLibrary, float(libraryPos(0)), float(libraryPos(2))}});
        check(reach[0] && reach[1] && reach[2] && !reach[3], "reach: automation between two libraries passes the ones in between");
        const int sourceChoices = int(specOf(DeviceType::DrumStudio)->params[size_t(ds::contentSource(ds::Snare))].choices.size());
        p[ds::contentSource(ds::Snare)] = float(choicePosition(2 + 1, sourceChoices));   // the snare from B
        reach = audio::drums::reachableBanks(p.data(), {});
        check(reach[0] && reach[1] && !reach[2], "reach: a drum's own library is loaded besides the kit's");
        for (int d = 0; d < ds::kDrums; ++d) p[ds::contentSource(d)] = float(choicePosition(1, sourceChoices));   // every drum: the built-in recipe
        reach = audio::drums::reachableBanks(p.data(), {});
        check(!reach[0] && !reach[1], "reach: drums that all play the built-in kit need no library");
    }

    // ---- the live engine never waits for a library
    Project p;
    p.scenes = {"1"};
    p.addTrack("Drums", QColor("#c08a5a"));
    p.tracks[0].devices = {p.makeDevice("Drum Studio", DeviceKind::Instrument)};
    const int deviceId = p.tracks[0].devices[0].id;
    auto setLibrary = [&](int bank) { p.tracks[0].devices[0].params[ds::ContentLibrary].value = libraryPos(bank); };
    audio::Engine engine(&p);
    Transport transport(&p, &engine);
    const double rate = engine.sampleRate();
    engine.setBackgroundContent(true);
    settle(engine);
    const auto builtIn = hit(engine, transport);
    check(heard(builtIn, rate) == Heard::Neither, "setup: the built-in kit is neither generated tone");

    loader->setReadDelayForTests(5);   // 60 hits: about 300 ms a library
    setLibrary(0);
    QElapsedTimer timer; timer.start();
    engine.publish();
    const qint64 publishMs = timer.elapsed();
    const auto state0 = loader->status(0).state;
    check(publishMs < 100 && (state0 == Loader::State::Queued || state0 == Loader::State::Loading), "load: choosing a library doesn't wait for it (it loads in the background)");
    const auto during = hit(engine, transport);
    check(heard(during, rate) == Heard::Neither && tone(during, 300, rate) < 1e-3, "load: meanwhile the drum plays the built-in kit, not silence");
    bool midway = false;
    QSignalSpy progressed(loader, &Loader::progressed), finished(loader, &Loader::finished);
    while (loader->status(0).state != Loader::State::Ready && timer.elapsed() < 10000) {
        const double f = loader->status(0).progress;
        midway |= f > 0 && f < 1;
        QApplication::processEvents(); QTest::qWait(10);
    }
    check(midway && progressed.size() >= 3, "load: progress is reported along the way");
    settle(engine);
    const auto status0 = loader->status(0);
    check(status0.state == Loader::State::Ready && status0.progress == 1 && status0.bytes > 0 && status0.error.isEmpty() && finished.size() >= 1,
          "load: the library is ready, with its size");
    const auto withA = hit(engine, transport);
    check(heard(withA, rate) == Heard::A, "load: once it's ready the engine republishes and the recorded kit plays");

    // ---- switching keeps the previous kit until the new one is ready
    setLibrary(1);
    engine.publish();
    check(loader->status(1).state == Loader::State::Queued || loader->status(1).state == Loader::State::Loading, "switch: the new library loads in the background");
    check(heard(hit(engine, transport), rate) == Heard::A, "switch: until it's ready, the drum plays the kit it played before");
    loader->waitIdle(10000);
    settle(engine);
    check(heard(hit(engine, transport), rate) == Heard::B, "switch: then the new kit plays");

    // ---- kept within the budget, released beyond it, cancelled when no longer wanted
    check(loader->status(0).state == Loader::State::Ready && loader->ready(0), "budget: with room, the kit switched away from stays ready");
    setLibrary(0);
    timer.restart();
    engine.publish();
    check(loader->status(0).state == Loader::State::Ready && timer.elapsed() < 100, "budget: switching back to it needs no loading");
    settle(engine);
    check(heard(hit(engine, transport), rate) == Heard::A, "budget: and it plays at once");
    setLibrary(1); engine.publish(); settle(engine);
    loader->setBudget(1);   // no room: only kits in use stay
    settle(engine);
    check(loader->status(0).state == Loader::State::Idle && !loader->ready(0) && loader->memoryUsed() == loader->status(1).bytes,
          "budget: without room, the kit no longer in use is let go");
    loader->setReadDelayForTests(20);   // 1.2 s: time to change one's mind
    setLibrary(0); engine.publish();
    QTest::qWait(60);
    const bool started = loader->status(0).state == Loader::State::Loading;
    setLibrary(1); engine.publish();
    loader->waitIdle(10000);
    settle(engine);
    check(started && loader->status(0).state == Loader::State::Idle && !loader->ready(0) && loader->memoryUsed() == loader->status(1).bytes,
          "cancel: a library switched away from while it loads is abandoned");
    check(heard(hit(engine, transport), rate) == Heard::B, "cancel: the kit in use plays on");
    loader->setBudget(0);
    loader->setReadDelayForTests(0);

    // ---- a library that can't load says why; one that isn't installed says so
    setLibrary(2); engine.publish(); loader->waitIdle(10000); settle(engine);
    const auto broken = loader->status(2);
    check(broken.state == Loader::State::Failed && broken.error.contains("invalid", Qt::CaseInsensitive), "fail: an unreadable library fails, with the reason");
    check(heard(hit(engine, transport), rate) == Heard::Neither, "fail: its drums play the built-in kit");
    check(loader->status(3).state == Loader::State::Missing && loader->status(5).state == Loader::State::Missing, "fail: a library that isn't installed is Missing");

    // ---- automation preloads only the libraries it reaches
    setLibrary(0);
    Envelope lane; lane.target = target::device(deviceId, ds::ContentLibrary);
    lane.points = {{0, libraryPos(0)}, {4, libraryPos(1)}};
    p.tracks[0].automation = {lane};
    engine.publish();
    check(loader->wanted(0) && loader->wanted(1) && !loader->wanted(2) && !loader->wanted(3), "automation: the libraries it moves between are loaded, not all six");
    loader->waitIdle(10000);
    p.tracks[0].automation.clear();
    engine.publish();
    settle(engine);
    check(!loader->wanted(1) && loader->wanted(0), "automation: without it, only the selected library is wanted");

    // ---- a kit arriving doesn't cut ringing drums; a kit replaced does
    {
        const auto a = loader->ready(0), b = loader->ready(1);
        check(a && b, "ringing: both generated kits are loaded");
        if (a && b) {
            auto proc = audio::createProcessor(DeviceType::DrumStudio, 48000);
            std::vector<float> params(size_t(ds::kParams));
            for (int i = 0; i < ds::kParams; ++i) params[size_t(i)] = float(p.tracks[0].devices[0].params[i].value);
            params[ds::ContentLibrary] = float(libraryPos(0));
            auto first = std::make_shared<audio::drums::Content>(); first->banks[0] = a;
            auto more = std::make_shared<audio::drums::Content>(); more->banks[0] = a; more->banks[1] = b;
            auto replaced = std::make_shared<audio::drums::Content>(); replaced->banks[0] = std::make_shared<audio::drums::ContentBank>(*a);
            audio::Context ctx; ctx.drumContent = first.get();
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            auto block = [&](bool note) { audio::EventList ev; if (note) ev.add({0, 38, 0.85f, 48000}); std::fill_n(l, audio::kMaxBlock, 0.f); std::fill_n(r, audio::kMaxBlock, 0.f);
                proc->render(ev, l, r, audio::kMaxBlock, params.data(), ctx); double e = 0; for (int i = 0; i < audio::kMaxBlock; ++i) e += l[i] * l[i]; return e; };
            block(true); block(false);
            ctx.drumContent = more.get();
            const double after = block(false);
            ctx.drumContent = replaced.get();
            block(false);
            const double cut = block(false);
            check(after > 1e-4, "ringing: a library arriving leaves the drum that's sounding alone");
            check(cut < after * 1e-3, "ringing: a library replaced stops the voices that played it (no dangling hits)");
        }
    }

    // ---- Drum Studio shows what's loading
    {
        loader->setBudget(1);
        setLibrary(1); engine.publish(); settle(engine);   // A let go again
        loader->setReadDelayForTests(20);
        DeviceView view(&p, &transport);
        view.resize(1480, 780); view.show(); view.rebuild(); QApplication::processEvents();
        view.expandDevice(deviceId); QApplication::processEvents();
        setLibrary(0); engine.publish();
        QTest::qWait(120);
        auto* info = view.findChild<QLabel*>("studioSourceInfo");
        const QString loading = info ? info->text() : QString();
        if (!shots.isEmpty()) { QDir().mkpath(shots); view.grab().save(shots + "/drum-loading-editor.png"); }
        check(loading.contains("Loading Big Rusty Drums") && loading.contains("%"), "panel: the editor's status line shows the library loading, with progress");
        loader->waitIdle(10000); settle(engine); QTest::qWait(60);
        check(info && !info->text().contains("Loading"), "panel: and clears when it's ready");
        setLibrary(2); engine.publish(); loader->waitIdle(10000); settle(engine); QTest::qWait(60);
        check(info && info->text().contains("couldn't load"), "panel: a library that failed says so");
        setLibrary(3); emit p.deviceChanged(); engine.publish(); settle(engine); QTest::qWait(60);   // as choosing it in the panel does
        check(info && info->text().contains("DRSKit isn't installed"), "panel: a missing library says so");
        if (!shots.isEmpty()) view.grab().save(shots + "/drum-loading-missing.png");
        loader->setReadDelayForTests(0);
        loader->setBudget(0);
    }

    // ---- offline renders and tests still decode when they publish
    {
        loader->setBudget(1);
        setLibrary(1); engine.publish(); settle(engine);
        check(!loader->ready(0), "blocking: setup, the first library isn't loaded");
        Project q; q.scenes = {"1"}; q.addTrack("Drums", QColor("#c08a5a"));
        q.tracks[0].devices = {q.makeDevice("Drum Studio", DeviceKind::Instrument)};
        q.tracks[0].devices[0].params[ds::ContentLibrary].value = libraryPos(0);
        audio::Engine offline(&q);
        Transport offlineTransport(&q, &offline);
        offline.publish();
        check(!offline.backgroundContent() && loader->ready(0), "blocking: an offline engine has the library as soon as it publishes");
        check(heard(hit(offline, offlineTransport), rate) == Heard::A, "blocking: and plays it");
        loader->setBudget(0);
    }

    check(allocations == 0, "audio callbacks allocate nothing while libraries load, switch and go");
    std::printf("drum_loading: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
