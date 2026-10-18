#pragma once
// Scales for the note editor's highlighting and Fold. Each scale is a
// 12-bit mask of the semitones above the root that belong to it.

namespace sessions {

struct ScaleDef { const char* name; unsigned mask; };

inline constexpr ScaleDef kScales[] = {
    {"Major",            0b101010110101},
    {"Minor",            0b010110101101},
    {"Dorian",           0b011010101101},
    {"Phrygian",         0b010110101011},
    {"Lydian",           0b101011010101},
    {"Mixolydian",       0b011010110101},
    {"Locrian",          0b010101101011},
    {"Harmonic Minor",   0b100110101101},
    {"Melodic Minor",    0b101010101101},
    {"Major Pentatonic", 0b001010010101},
    {"Minor Pentatonic", 0b010010101001},
    {"Blues",            0b010011101001},
};
inline constexpr int kScaleCount = int(sizeof(kScales) / sizeof(kScales[0]));

inline const char* noteName(int pc) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[((pc % 12) + 12) % 12];
}

inline bool inScale(int pitch, int root, int scale) {
    if (scale < 0 || scale >= kScaleCount) return true;
    int step = ((pitch - root) % 12 + 12) % 12;
    return (kScales[scale].mask >> step) & 1u;
}

} // namespace sessions
