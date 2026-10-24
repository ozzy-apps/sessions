# Production devices · 0.48

Sessions remains a native C++17 / Qt 6 Widgets application. These devices appear in the existing browser and device chain. All ordinary controls support automation, MIDI mapping and set/preset saving. Older EQs keep their original behavior.

## Mixing and modulation

- **Parametric EQ:** eight bands with bell, shelves, low/high cut, notch and band-pass filters. Each band can address stereo, left, right, mid or side. Cut slopes are 12, 24 or 48 dB/octave. Select a numbered band, drag its dot for frequency/gain, or use the controls below; the wheel changes Q. Audition isolates a band's frequency range. Analyze shows measured input and output spectra.
- **Spectrum:** a stereo-energy analyzer with 256 logarithmic display bins, a 2048-sample Hann window, adjustable decay smoothing and Hold. Anti-phase stereo signals remain visible. It does not change audio.
- **Multiband Dynamics:** low/mid/high bands with aligned fourth-order Linkwitz–Riley crossovers. Above ratios over 1 compress peaks; ratios under 1 expand peaks. Below ratios under 1 raise quiet signals; ratios over 1 expand downward. Set crossovers, thresholds, ratios, attack/release, band gain and solo. Amount scales dynamics; Dry/Wet blends against the phase-aligned unprocessed reconstruction.
- **LFO:** synced or free-running sine, triangle, ramps, square and sample-and-hold modulation, with gain and independent rise/fall smoothing.
- **Envelope Follower:** input level to a mapped value, with Rise, Fall, Gain and optional external-track sidechain/filter/listen controls.
- **Shaper:** draw a curve in its display. The 33 points, including the endpoint, follow the selected synced/free rate. Direction supports forward, reverse, ping-pong, steps, random and sine traversal, followed by gain and rise/fall smoothing. An empty curve is a rising ramp.

Use a modulator's **Map** menu to choose another device's parameter or the track mixer. Each row sets a minimum and maximum; reversing them inverts the motion. Mixer targets include regular-track sends, track/return pan and volume, and Master volume. Bypassing the source releases its mappings. A source before its target affects the current 128-sample block; a source after its target affects the next block. Modulators use absolute mapping ranges. Manual base values are retained.

## Pitch and space

- **Shifter:** pitch shift ±24 semitones with cents and formants, formant preservation, frequency shifting and ring modulation. Raising correction Strength enables the root/scale quantizer in pitch mode; it starts at zero in Shifter and full strength in Pitch Corrector. Mixing a shifted signal with dry audio creates two-voice harmony. Spectral pitch processing uses a 2048-sample window (about 42.7 ms at 48 kHz); the dry branch is aligned. Frequency/ring modes use a 129-tap Hilbert filter and a 64-sample dry delay. This implementation does not provide host-wide plugin delay compensation.
- **Pitch Corrector:** monophonic pitch tracking to chromatic, major, minor or major-pentatonic scales. Set root, strength and retune time, then adjust transposition/formants as needed. Unvoiced/silent input is not pitch corrected. This is not a polyphonic audio editor or MIDI harmonizer.
- **Hybrid Reverb:** an eight-line feedback-delay reverb plus convolution with an imported room/plate IR, up to eight seconds. Choose algorithm, convolution, parallel or serial routing. Predelay, damping, wet EQ, width, freeze, blend and output controls shape the result. Load IR also switches to parallel routing. Convolution adds 128 samples of latency; its dry branch is aligned. The existing Cabinet/NAM IR limit and sound are unchanged. Long IRs cost more CPU; preparation and storage allocation happen outside rendering.

## Looper

Record, Play, Stop and Overdub have separate MIDI-mappable trigger parameters. Choose a fixed length or Free. Recording starts at the next bar when the transport runs; fixed lengths use the tempo at record start, up to 120 seconds. During Record and Overdub, incoming audio is monitored. Feedback controls overdub retention. Reverse and Half Speed apply during playback; overdubbing runs forward at normal speed. Undo restores the previous overdub operation. Restoration and export copy bounded chunks without allocating on the audio thread.

**Unsaved loop** means the current audio exists in memory. Stop overdubbing, then **Save Loop** to write a 32-bit float WAV and attach it to the device. Save the set afterwards; the WAV participates in the existing relative-path and Collect All and Save workflow. Saved loops reopen stopped, with transport triggers cleared; press Play to resume. New live recordings are not automatically written by ordinary set save or recovery. This explicit step also applies after further overdubs or undo. Clear removes the panel's saved sample reference. Looper playback does not time-stretch if the set tempo changes after recording.

## Multisampler

Import audio files or a supported SFZ mapping. File decoding runs in the background. Audio filenames containing notes such as `A4` supply a root note; distinct roots are split at their midpoints. Files without a recognized note default to MIDI 60. Overlapping key/velocity zones layer unless their round-robin positions select alternatives. The instrument supports 128 zones and 32 simultaneous voices, with ADSR, volume/tune, filter and looping.

**Expand** opens the zone table. Edit key/velocity limits, root (MIDI note numbers), gain, pan (−1..1), tune in semitones, loop bounds in source frames, round-robin sequence length/position and one-shot behavior. Missing samples appear in the missing-file color. Zone edits support undo/redo and set/preset saving.

The SFZ importer supports `<control>` default paths and `<global>` / `<group>` / `<region>` inheritance for `sample`, `key`, `lokey`, `hikey`, `lovel`, `hivel`, `pitch_keycenter`, `volume`, `pan`, `tune`, `transpose`, `loop_start`, `loop_end`, `loop_mode`, `seq_length` and `seq_position`. Quoted and unquoted sample paths with spaces work. Unsupported headers/opcodes, includes/macros and built-in oscillators are rejected explicitly. SF2, SF3, Kontakt and full SFZ synthesis/controller opcodes are not implemented. Reference: [SFZ opcode specification](https://sfzformat.com/opcodes/).

## Verification

`sessions_production_devices` checks registry/save compatibility, DSP measurements, sample rates, callback allocations and compact-panel layout. `sessions_studio_devices` checks rendered pitch/frequency/ring shifts, correction, late IR reflections, predelay, looper capture/overdub/undo, key/velocity/round-robin selection, SFZ inheritance, zone-editor undo and engine modulation routing. Both accept an output directory for captures. Use `QT_QPA_PLATFORM=offscreen SESSIONS_AUDIO=none` for isolated checks.
