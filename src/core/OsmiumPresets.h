#pragma once
#include "DeviceSpecs.h"
#include <array>

namespace sessions::osmium {

// Factory data only: no specOf() calls while building the bank (DeviceSpecs uses it).
struct FactoryPreset {
    const char* name;
    const char* category;
    const char* description;
    std::array<double, kParams> values{};
    QVector<ModRoute> mods;
    QVector<QVector<CurvePoint>> curves;
    double bpm = 174;   // the tempo it was voiced at (its phrases follow any tempo)
};
const std::vector<FactoryPreset>& factoryPresets();
const FactoryPreset* factoryPreset(const QString& name);
inline constexpr const char* kMacroNames[8] = {"Tone", "Warp", "Drive", "Width", "Space", "Echo", "Body", "Air"};
void applyPreset(Device& device, const FactoryPreset& preset); // preserves identity/name
}
