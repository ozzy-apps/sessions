#pragma once
// Built-in sounds: one-shots rendered from the synthesized drum kits, so a new Drum Rack
// plays straight away. They're samples like any other, named by a path the set file
// stores as is: "builtin:<kit>/<note>", kit = kit / 808 / acoustic, note = the GM drum note.
// Rendered once on the UI thread and kept for the session.

#include "core/Sample.h"
#include <QString>
#include <QStringList>
#include <QVector>

namespace sessions {

struct DrumPad;

namespace audio {

bool      isBuiltin(const QString& path);
SamplePtr builtinSample(const QString& path);   // nullptr for a name it doesn't know
QString   builtinName(const QString& path);     // "Kick", "Hat Closed", …; an IR's "4x12 Closed · On Axis"

// The built-in impulse responses (0.19): the Guitar Amp's five cabinets through each mic,
// "builtin:ir/<cab>/<mic>" (cab 1 .. 5, mic 0 on axis / 1 off axis / 2 far), 15 in all, for the
// browser's Impulse Responses and a Cabinet device's library. Rendered once (170 ms at 48 kHz).
QStringList builtinIrPaths();
bool        isBuiltinIr(const QString& path);

// The Drum Rack a new one starts with: the 808 kit's sounds on C1 to D#2.
QVector<DrumPad> defaultDrumPads();

} // namespace audio
} // namespace sessions
