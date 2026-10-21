# Stem tempo investigation · 0.38.1

The reported playback problem reproduces on the unmodified 0.38.0 detector. This is a source-timing error before the audio reaches the stretching algorithm.

| `_06` stem | Old source BPM | Playback speed at project 176 |
| --- | ---: | ---: |
| Drums | 115.995542 | 1.517× recorded speed |
| Bass | 231.015893 | 0.762× |
| Keyboard | Unknown | Recorded speed, Warp off |
| Synth | 132.345249 | 1.330× |
| Other | 220.950491 | 0.797× |

All five recordings are stereo 48 kHz, exactly 13,008,000 frames / 271 seconds. `_01` has four matching recordings of 10,748,160 frames / 223.92 seconds. Neither export supplies an explicit BPM tag.

## Measured source timing

An independent analysis using stereo spectral flux, several frequency bands and multiple passages supports approximately 174 BPM throughout `_06`. The old 116 BPM reading is a two-thirds metrical alias. It has strong repeating-pattern correlation without being the track's beat grid.

`_01` changes tempo: around 174 initially, 174.4 near 80 seconds, 176 near 120 seconds, 181.5 around 176–184 seconds and approximately 183 near the end. Its percussion also supports the corresponding half-time pulse. The drumless passage around 136–168 seconds provides little direct timing evidence. A generation prompt requesting 176/178 BPM does not establish an exact tempo for these exported recordings.

The final detector reports `_06` drums at **173.997298 BPM** and safely leaves its ambiguous isolated stems unknown. `_01` drums report **174.148413 BPM** over the initial analysis region; percussion alone reports **87.017460 BPM**. The previously supplied `_03` drum recording remains **172.000108 BPM**, providing another actual-file regression.

## Implementation

- **Detector:** fractional-period candidate search, eighth/sixteenth subdivisions and longer phrase support correct the demonstrated aliases. A separate attack envelope suppresses bass-carrier ripple; local contrast and agreement across passages reject weak periodic textures. Confidence measures evidence, not a calibrated probability of the exact meter.
- **Stem imports:** long matching exports share one rhythmic guide and its exact map. Separately imported recognized stem names can join already loaded siblings. Project tempo does not determine source tempo. No unrequested audio is scanned. Persistent provenance and map fingerprints protect manual, legacy and edited timing.
- **Changing tempo:** `BeatGrid` measures supported local periods, smooths broad changes, integrates from the original file start and checks lower and higher beat-phase harmonics. Lower harmonics prevent a sixteenth-note count slip across the break. Ambiguous gaps can cause analysis to abstain. Markers describe broad tempo changes, rather than quantizing every transient.
- **Repairs:** explicit re-detection can replace a wrong saved positive estimate. Shared corrections are undoable and preserve source-time coordinates for placements, fades, envelopes and take offsets. Normal Sync preserves accepted variable maps and trimmed source ends.

Both actual folders pass batch and reverse-order individual import checks. Every `_06` stem receives the same two-point 173.997298 BPM map. Every `_01` stem receives the same **32-point** map, with average **177.079946 BPM** and the original start/end retained. Changing project tempo leaves those source maps unchanged.

On this host, batch analysis took approximately **0.59 seconds** for the five `_06` stems and **0.45 seconds** for `_01`, excluding decode. These are local measurements, not real-time performance guarantees.

## Verification

All **18 combined CTest suites passed**. The 109 added checks cover detector aliases, uncertain material, changing-tempo grids, shared imports, source protection, repair UI, serialization/undo and Engine playback through 41 nonuniform markers. Generated acceleration predicts withheld beat times within **2.15 ms**, versus over three seconds of drift with the wrong constant grid. Independent spectral-phase measurements of the actual `_01` map confirm consistent phase before and after its drumless break; they do not establish ground truth inside that inaudible passage.

No source recordings are repository fixtures. Tests generate their own audio. Actual-file reports and exploratory scripts remain local under `/tmp/sessions-stem-*` and `/tmp/sessions381-*`.

For use and repairs, see [Source tempo and synchronized stems](tempo-stems.md). The shared-map approach also agrees with the documented [equal-length multitrack import workflow](https://www.bitwig.com/userguide/latest/working_with_audio_events/).
