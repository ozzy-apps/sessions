#include "SampleZones.h"
#include "DeviceSpecs.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace sessions::samplezones {
namespace {
int note(QString text, int fallback = 60) {
    bool ok = false;
    int midi = text.toInt(&ok);
    if (ok)
        return std::clamp(midi, 0, 127);
    QRegularExpression re("^([A-Ga-g])([#b]?)(-?[0-9]+)$");
    auto m = re.match(text);
    if (!m.hasMatch())
        return fallback;
    int pc = QString("C D EF G A B").indexOf(m.captured(1).toUpper());
    int n = (m.captured(3).toInt() + 1) * 12 + pc + (m.captured(2) == "#" ? 1 : m.captured(2) == "b" ? -1 : 0);
    return std::clamp(n, 0, 127);
}
} // namespace
QVector<SampleZone> mapAudioFiles(const QStringList& paths) {
    QVector<SampleZone> out;
    QRegularExpression re("(?:^|[_ \\-])([A-Ga-g][#b]?-?[0-9])(?:$|[_ \\-])");
    for (const auto& path : paths) {
        if (out.size() >= multisampler::kMaxZones)
            break;
        SampleZone z;
        z.samplePath = QFileInfo(path).absoluteFilePath();
        auto m = re.match(QFileInfo(path).completeBaseName());
        if (m.hasMatch())
            z.root = note(m.captured(1));
        out.push_back(z);
    }
    QVector<int> roots;
    for (const auto& z : out)
        if (!roots.contains(z.root))
            roots.push_back(z.root);
    std::sort(roots.begin(), roots.end());
    for (auto& z : out) {
        int i = roots.indexOf(z.root);
        z.keyLow = i ? ((roots[i - 1] + z.root) / 2 + 1) : 0;
        z.keyHigh = i + 1 < roots.size() ? (z.root + roots[i + 1]) / 2 : 127;
    }
    return out;
}
bool readSfz(const QString& path, QVector<SampleZone>& result, QString* error) {
    auto fail = [&](QString why) {
        if (error)
            *error = why;
        return false;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > 4 * 1024 * 1024)
        return fail("SFZ exceeds 4 MB");
    QString text = QString::fromUtf8(file.readAll());
    text.remove(QRegularExpression("//[^\\n]*"));
    if (text.contains("#include") || text.contains("#define"))
        return fail("SFZ includes and macros are not supported");
    QRegularExpression tokens(
        "(<[^>]+>)|([A-Za-z_][A-Za-z_0-9]*)\\s*=\\s*(\"[^\"]*\"|.*?)(?=\\s+[A-Za-z_][A-Za-z_0-9]*\\s*=|\\s*<|$)",
        QRegularExpression::DotMatchesEverythingOption);
    QHash<QString, QString> global, group, region, control;
    QHash<QString, QString>* current = &global;
    QString header = "global";
    QVector<SampleZone> zones;
    QString bad;
    auto flush = [&] {
        if (header != "region" || !region.contains("sample"))
            return;
        SampleZone z;
        auto value = [&](const char* k, QString def = {}) { return region.value(k, def); };
        QString name = value("sample");
        if (name.startsWith('*')) {
            bad = "SFZ built-in oscillators are not supported";
            return;
        }
        name.replace('\\', '/');
        QString base = QFileInfo(path).absolutePath();
        QString defaultPath = control.value("default_path");
        defaultPath.replace('\\', '/');
        z.samplePath = QFileInfo(QDir(base).filePath(QDir(defaultPath).filePath(name))).absoluteFilePath();
        int key = note(value("key", "60"));
        z.root = note(value("pitch_keycenter", QString::number(key)));
        z.keyLow = note(value("lokey", region.contains("key") ? QString::number(key) : "0"), 0);
        z.keyHigh =
            std::max(z.keyLow, note(value("hikey", region.contains("key") ? QString::number(key) : "127"), 127));
        z.velocityLow = std::clamp(value("lovel", "1").toInt(), 1, 127);
        z.velocityHigh = std::clamp(value("hivel", "127").toInt(), z.velocityLow, 127);
        z.gain = std::clamp(value("volume", "0").toDouble(), -60., 24.);
        z.pan = std::clamp(value("pan", "0").toDouble() / 100, -1., 1.);
        z.tune = std::clamp(value("transpose", "0").toDouble() + value("tune", "0").toDouble() / 100, -24., 24.);
        z.loopStart = std::max(0., value("loop_start", "0").toDouble());
        z.loopEnd = region.contains("loop_end") ? std::max(z.loopStart, value("loop_end").toDouble() + 1) : 0;
        QString loop = value("loop_mode", "no_loop");
        z.loop = loop == "loop_continuous" || loop == "loop_sustain";
        z.oneShot = loop == "one_shot";
        z.sequenceLength = std::clamp(value("seq_length", "1").toInt(), 1, 100);
        z.sequencePosition = std::clamp(value("seq_position", "1").toInt(), 1, z.sequenceLength);
        zones.push_back(z);
    };
    const QSet<QString> supported = {
        "sample",          "key",       "lokey",      "hikey",        "lovel",       "hivel",
        "pitch_keycenter", "volume",    "pan",        "tune",         "transpose",   "loop_start",
        "loop_end",        "loop_mode", "seq_length", "seq_position", "default_path"};
    auto it = tokens.globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        if (!m.captured(1).isEmpty()) {
            flush();
            header = m.captured(1).mid(1, m.captured(1).size() - 2).trimmed();
            if (header == "global") {
                global.clear();
                group.clear();
                current = &global;
            } else if (header == "group") {
                group = global;
                current = &group;
            } else if (header == "region") {
                region = group.isEmpty() ? global : group;
                current = &region;
            } else if (header == "control")
                current = &control;
            else
                return fail("Unsupported SFZ header: " + header);
        } else {
            QString k = m.captured(2), v = m.captured(3).trimmed();
            if (v.startsWith('"') && v.endsWith('"'))
                v = v.mid(1, v.size() - 2);
            if (!supported.contains(k))
                return fail("Unsupported SFZ opcode: " + k);
            current->insert(k, v);
            if (k == "key") { // SFZ key is shorthand for all three, at this header's precedence.
                current->insert("lokey", v);
                current->insert("hikey", v);
                current->insert("pitch_keycenter", v);
            }
        }
        if (zones.size() > multisampler::kMaxZones)
            return fail("SFZ exceeds 128 regions");
    }
    flush();
    if (!bad.isEmpty())
        return fail(bad);
    if (zones.isEmpty())
        return fail("SFZ has no sample regions");
    if (zones.size() > multisampler::kMaxZones)
        return fail("SFZ exceeds 128 regions");
    result = zones;
    return true;
}
} // namespace sessions::samplezones
