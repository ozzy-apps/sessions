#pragma once
// Audio-clip import defaults live outside a set. Existing clips and saved sets
// keep their own mode; only Project::newAudioClip reads this preference.

#include "Model.h"
#include <QSettings>

namespace sessions::clipsettings {

inline constexpr char kDefaultWarpMode[] = "clips/defaultWarpMode";

inline bool validWarpMode(int mode) {
    return mode >= int(WarpMode::Beats) && mode <= int(WarpMode::ComplexPro);
}

inline WarpMode defaultWarpMode() {
    bool ok = false;
    const int value = QSettings("Sessions", "Sessions").value(kDefaultWarpMode, int(WarpMode::Beats)).toString().toInt(&ok);
    return ok && validWarpMode(value) ? WarpMode(value) : WarpMode::Beats;
}

inline void setDefaultWarpMode(WarpMode mode) {
    QSettings("Sessions", "Sessions").setValue(kDefaultWarpMode, validWarpMode(int(mode)) ? int(mode) : int(WarpMode::Beats));
}

} // namespace sessions::clipsettings
