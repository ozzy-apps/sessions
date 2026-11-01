#include "WaveIO.h"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstring>
namespace sessions::stems {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
std::vector<float> readInputWave(const QString& path) {
    QFile input(path);
    require(input.open(QIODevice::ReadOnly), "Couldn't open the prepared input WAV.");
    const QByteArray header = input.read(12);
    require(header.size() == 12 && header.startsWith("RIFF") && header.mid(8) == "WAVE", "Invalid input WAV.");
    bool format = false;
    qint64 offset = -1, bytes = 0;
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(8);
        if (chunk.size() != 8) break;
        const quint32 length = qFromLittleEndian<quint32>(chunk.constData() + 4);
        const qint64 at = input.pos();
        require(length <= input.size() - at, "Truncated input WAV.");
        if (chunk.startsWith("fmt ")) {
            const QByteArray fmt = input.read(std::min<quint32>(length, 40));
            const bool floating = fmt.size() >= 16 && (qFromLittleEndian<quint16>(fmt.constData()) == 3
                || (fmt.size() >= 40 && qFromLittleEndian<quint16>(fmt.constData()) == 0xfffe
                    && qFromLittleEndian<quint16>(fmt.constData() + 24) == 3));
            format = floating
                && qFromLittleEndian<quint16>(fmt.constData() + 2) == 2
                && qFromLittleEndian<quint32>(fmt.constData() + 4) == 44100
                && qFromLittleEndian<quint16>(fmt.constData() + 14) == 32;
        } else if (chunk.startsWith("data")) { offset = at; bytes = length; }
        require(input.seek(at + length + (length & 1)), "Couldn't read input WAV chunks.");
    }
    require(format && offset >= 0 && bytes > 0 && bytes % 8 == 0, "Input must be a 44.1 kHz stereo float WAV.");
    require(bytes / 8 <= 44100LL * 3600, "The input exceeds the one-hour separation limit.");
    require(input.seek(offset), "Couldn't read WAV audio.");
    std::vector<float> result(size_t(bytes / sizeof(float)));
    qint64 done = 0;
    while (done < bytes) {
        const QByteArray block = input.read(std::min<qint64>(65536, bytes - done));
        require(!block.isEmpty(), "Truncated input audio.");
        for (int i = 0; i < block.size() / 4; ++i) {
            const float value = qFromLittleEndian<float>(block.constData() + i * 4);
            require(std::isfinite(value), "Input contains invalid audio samples.");
            result[size_t(done / 4) + i] = value;
        }
        done += block.size();
    }
    return result;
}
FloatWave::FloatWave(const QString& path) : file(path) {
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Couldn't create a stem WAV.");
    require(file.write(QByteArray(44, '\0')) == 44, "Couldn't write a stem WAV header.");
}
void FloatWave::add(float left, float right) {
    require(std::isfinite(left) && std::isfinite(right), "Non-finite separated audio.");
    pending[size_t(used++)] = left; pending[size_t(used++)] = right; ++frames;
    if (used == int(pending.size())) flush();
}
void FloatWave::flush() {
    QByteArray buffer(used * 4, '\0');
    for (int i = 0; i < used; ++i) qToLittleEndian<float>(pending[size_t(i)], buffer.data() + i * 4);
    require(file.write(buffer) == buffer.size(), "Couldn't write the stem audio. Check free disk space.");
    used = 0;
}
void FloatWave::finish(qint64 expected) {
    require(frames == expected, "A stem has an incorrect duration.");
    flush();
    require(frames <= (UINT32_MAX - 36) / 8, "Stem exceeds the WAV format limit.");
    QByteArray header(44, '\0');
    std::memcpy(header.data(), "RIFF", 4); qToLittleEndian<quint32>(36 + frames * 8, header.data() + 4);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8); qToLittleEndian<quint32>(16, header.data() + 16);
    qToLittleEndian<quint16>(3, header.data() + 20); qToLittleEndian<quint16>(2, header.data() + 22);
    qToLittleEndian<quint32>(44100, header.data() + 24); qToLittleEndian<quint32>(44100 * 8, header.data() + 28);
    qToLittleEndian<quint16>(8, header.data() + 32); qToLittleEndian<quint16>(32, header.data() + 34);
    std::memcpy(header.data() + 36, "data", 4); qToLittleEndian<quint32>(frames * 8, header.data() + 40);
    require(file.seek(0) && file.write(header) == 44 && file.flush(), "Couldn't finish the stem WAV.");
    file.close();
}
} // namespace sessions::stems
