#pragma once
#include "Devices.h"

namespace sessions::audio {
std::shared_ptr<DeviceProcessor> createProductionEffect(DeviceType type, float sampleRate);
std::shared_ptr<DeviceProcessor> createPitchEffect(DeviceType type, float sampleRate);
std::shared_ptr<DeviceProcessor> createSpatialEffect(DeviceType type, float sampleRate);
std::shared_ptr<DeviceProcessor> createLooper(float sampleRate);
std::shared_ptr<DeviceProcessor> createMultisampler(float sampleRate);
float parametricResponseDb(const float* params, float frequency, float sampleRate, int channel = 0);
void parametricResponseCurve(const float* params, float* response, int count, float sampleRate, int channel);
} // namespace sessions::audio
