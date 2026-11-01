#pragma once
#include <QFile>
#include <QString>
#include <array>
#include <vector>
namespace sessions::stems {
std::vector<float> readInputWave(const QString& file);
class FloatWave {
public:
    explicit FloatWave(const QString& path);
    void add(float left, float right);
    void finish(qint64 expectedFrames);
private:
    void flush();
    QFile file;
    std::array<float, 8192> pending{};
    int used = 0;
    qint64 frames = 0;
};
} // namespace sessions::stems
