# Audio recording inputs

In the Session mixer, use **Audio From** above an audio track's Monitor buttons:

- **In 1/2**, **In 1**, **In 2** select stereo or mono inputs from the interface selected in **Preferences → Audio → Input device**.
- A **track** or **group** selects its stereo output after effects, fades, pan, mute/solo and fader.
- **Master** records the complete mix, including returns and Master effects/fader/output protection. The metronome and browser preview are excluded.

Arm the destination and record into an empty Session slot, or enable Arrangement Record and Play. Each armed track records its own selected source. Monitor Off continues recording; In always monitors and Auto monitors while armed. Internal recording works with the interface input closed and adds no hardware capture/output latency correction.

Use Monitor Off when printing a track that is already audible in the mix, to avoid hearing both the source and the destination. Master and routes that would feed a group or track back into itself automatically disable input monitoring while retaining recording. The selector's tooltip identifies that condition. The destination's existing clips can still play when monitoring is disabled; stop or mute them if they should be excluded from the mix being recorded.

Input choices are saved with the set and support undo/redo. Tracks retain stable source identities through reordering and renaming. A deleted source shows **Missing source** and records silence. A take retains the source and hardware channel selected when that take began; changing the selector affects monitoring and the next take.

Capture uses source-specific bounded rings and keeps recent history for quantized launches and punch/loop recording. WAV writing and waveform history remain on the UI thread; audio callbacks only read the immutable route graph and copy into prepared buffers. The existing capture overflow counter reports a stalled recording consumer.
