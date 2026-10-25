#include "Driver.h"
namespace sessions::audio {
std::unique_ptr<Driver> makeJackDriver() {
    return {};
}
QStringList listJackPorts(bool, int*) {
    return {};
}
bool jackAvailable() {
    return false;
}
} // namespace sessions::audio
