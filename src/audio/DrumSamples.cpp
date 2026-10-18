#include "DrumSamples.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <cstring>

namespace sessions::audio::drums {
namespace {
struct Library {
    std::array<Bank, 3> banks;
    QString error;
    int loaded = 0;
    Library() {
        QString root = QCoreApplication::applicationDirPath() + "/../share/sessions/drums";
        if (!QFile::exists(root + "/library.json")) root = SESSIONS_DRUM_SOURCE_DIR;
        QFile manifest(root + "/library.json");
        if (!manifest.open(QIODevice::ReadOnly)) { error = "Drum recordings are missing"; return; }
        const auto entries = QJsonDocument::fromJson(manifest.readAll()).object()["banks"].toArray();
        for (int b = 0; b < entries.size() && b < 3; ++b) {
            const auto notes = entries[b].toObject()["notes"].toObject();
            for (auto it = notes.begin(); it != notes.end(); ++it) {
                const int note = it.key().toInt(); if (note < 0 || note > 127) continue;
                const auto files = it.value().toArray();
                for (int v = 0; v < files.size() && v < 5; ++v) {
                    QFile f(root + '/' + files[v].toString());
                    if (!f.open(QIODevice::ReadOnly)) { error = "Missing drum recording: " + f.fileName(); continue; }
                    const auto bytes = f.readAll();
                    if (bytes.size() < 44 || bytes.first(4) != "RIFF" || bytes.mid(8, 4) != "WAVE") { error = "Invalid drum WAV"; continue; }
                    int channels = 0, bits = 0, format = 0, rate = 0, data = 0, size = 0;
                    for (qint64 p = 12; p + 8 <= bytes.size();) {
                        const quint32 n = qFromLittleEndian<quint32>(bytes.constData() + p + 4);
                        if (p + 8 + n > bytes.size()) break;
                        if (bytes.mid(p, 4) == "fmt " && n >= 16) {
                            format = qFromLittleEndian<quint16>(bytes.constData() + p + 8);
                            channels = qFromLittleEndian<quint16>(bytes.constData() + p + 10);
                            rate = int(qFromLittleEndian<quint32>(bytes.constData() + p + 12));
                            bits = qFromLittleEndian<quint16>(bytes.constData() + p + 22);
                        } else if (bytes.mid(p, 4) == "data") { data = int(p + 8); size = int(n); }
                        p += 8 + n + (n & 1);
                    }
                    if (format != 1 || bits != 16 || channels != 1 || rate < 8000 || !data) { error = "Unsupported bundled drum WAV"; continue; }
                    auto& layer = banks[b].notes[note][v]; layer.rate = rate; layer.mono.resize(size / 2);
                    for (int i = 0; i < size / 2; ++i) layer.mono[i] = qFromLittleEndian<qint16>(bytes.constData() + data + i * 2) / 32768.f;
                    ++loaded;
                }
            }
        }
    }
};
const Library& data() { static const Library d; return d; }
}
const std::array<Bank, 3>& library() { return data().banks; }
int bankFor(int recipe) { constexpr int bank[6] = {0, 1, 0, 1, 2, 1}; return bank[recipe >= 0 && recipe < 6 ? recipe : 0]; }
const char* sourceName(int recipe) { constexpr const char* names[] = {"Black Pearl", "Red Zeppelin", "Blonde Bop"}; return names[bankFor(recipe)]; }
int sampleNote(int gm) { return gm == 35 ? 36 : (gm == 48 || gm == 50) ? 45 : gm; }
float sampleTune(int gm) { return (gm == 48 || gm == 50) ? 5.f : 0.f; }
QString error() { return data().error; }
int loadedLayers() { return data().loaded; }
}
