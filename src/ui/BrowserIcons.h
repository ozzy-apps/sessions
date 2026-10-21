#pragma once
#include <QIcon>

namespace sessions::browsericons {
// Original SVGs embedded in resources; icons inherit the current theme at render time.
enum class Symbol {
    Sounds, Drums, Instrument, AudioEffect, Guitar, MidiEffect, Impulse, Library,
    Folder, FolderOpen, AddFolder, Project, Sample, Capture, Clip, Preset, Rack,
    Sampler, Wavetable, Fm, Oscillator, Search, Headphones, ChevronRight, ChevronDown, Close, Stop
};
QIcon icon(Symbol symbol, bool toggle = false);
QIcon folder(); // closed / expanded states
}
