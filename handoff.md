# Sessions — handoff

## Milestone 32 · the Sampler reworked · 2026-10-18

Complete, Sessions **0.32.0**, built on its own branch (`sampler`) from 0.31.0 (`ec504a7`) while Drum Buss and the Osmium usability pass went into main, then merged with 0.31.1 (`ed26edc`). The user asked for a good sampler after the classic three-mode sampler panels: a waveform across the top with the sample's name, the three modes stacked at the left, a strip of controls under the waveform that changes with the mode, then Filter, LFO and the envelope. The device is still **Sampler** (same name, the 0.4 parameters 0 .. 12 keep their indices and meaning, so saved sets load as they did); everything new is appended (`sampler::` in `core/DeviceSpecs.h`, 40 parameters) or a new `Device` field.

1. **Three modes** (`Mode`). *Classic*: polyphonic from Root across the keyboard between Start and End, Loop on / off from **Loop Start** with a **Fade** (the end of the loop crossfades into the audio before its start), **Snap** (Start / End / Loop Start move to zero crossings), **Voices** 1 .. 16, **Retrig** (a key struck again while its note sounds restarts that voice). *1-Shot*: the whole region from every key; **Trigger** plays it out however short the key, **Gate** stops it with **Fade Out** when the key goes up; **Fade In** / **Fade Out** replace the envelope. *Slice*: **Slice By** Transient (the sample's own transients, those above **Sensitivity**: `SampleData::transientStrength`, one per transient, is new in `Sample.cpp`'s detector), Beat (every **Division** of the sample's tempo), Region (**Regions** equal parts) or Manual (`Device::slices`, seconds, sorted: double-click the waveform to add or remove one, drag one to move it; double-clicking in another mode switches to Manual keeping the slices as they were); the slices sit on the keys from C1 up; **Playback** Mono (a new slice chokes the one playing, 4 ms) / Poly (each slice to the next) / Thru (each plays on to End). `sampler::slices()` computes the list without allocating, so the processor calls it every block and the panel draws the same list.
2. **The waveform** (`SampleDisplay`): the ▶ start and ◀ end flags (drag them; a press within 8 px takes a flag, elsewhere nothing moves), in Classic the loop's flag, bar and crossfade shading, in Slice the numbered slices, a playhead per sounding voice (the processor writes every voice's position into `scope[0 .. 15]`), the sample's bars / tempo / length as a caption; "Drop a sample here" when empty, the missing file's name in `theme::devMissing`. Dragging the number band in Slice mode drags a MIDI clip of the slices out (`import::slicesToClip`: one note per slice from C1 at the slice's beat at the sample's tempo, written as a temporary `.sessionsclip` whose URL the Session View, the Arrangement and the browser already take).
3. **Filter / LFO / Amp**: a `FilterDisplay` with On, LP / HP / BP / Notch, Freq, Res and **Env** (the amplitude envelope's amount in octaves, ±6; filter On by default at 20 kHz, so old sets sound as before); one LFO (shape, Rate synced to the song or Free at the Hz knob, amounts to Pitch in semitones, Filter in octaves, Volume) with its own display (the line is `meter[1]`, the phase); the `AdsrDisplay` with A / D / S / R. Transpose, Detune (cents), Root and Vol < Vel at the right, Volume under the mode buttons.
4. **Warp**: `Device::sampleBpm` is found when a sample loads (`tempo::detect` in `import::toSampler`; 0 when there's no steady beat) and sits in the header's BPM field (a DragValue; `:2` / `x2` halve and double it, Delete clears it). With **Warp** on and a tempo known, the sample follows the set's tempo from its own: **Re-Pitch** scales the read speed (the pitch with it); **Tones / Texture / Complex / Complex Pro** run a `Stretcher` per voice (`audio/Stretch.h`, the audio clips' modes) fed the voice's position as a clip beat, so the pitch stays. Every voice keeps one clock, its position in sample frames, whichever path renders it: the loop, the fades and the playheads work the same with a Stretcher.
5. **Engine / model / file**: `Context::slices / sliceCount / sampleBpm` (the `DeviceSlot` carries a fixed array of 128 slices, like the Monolith's curves); `Device::sampleBpm` and `slices` are saved as `sampleBpm` and `slices` in `SetFile.cpp`, compared in `operator==(Device)` and so in the M4 round trip. `tests/smoke.cpp`'s M4 audio set now sets them on its Sampler and checks they come back.
6. **The panel** (`SamplerBody` in `InstrumentBodies.cpp`, 1130 × 192): `ModeColumn` (the stacked buttons, one `mapTarget`), the header (name, Warp, BPM field, `:2`, `x2`, Warp Mode chooser), the waveform, a `QStackedWidget` strip (Classic: Start / Loop / End / Fade knobs, Loop / Snap / Retrig, Voices; 1-Shot: Trigger / Gate, Fade In / Out; Slice: Slice By, then Sens, Division or Regions as the mode needs, Playback), Filter, LFO, Amp, the pitch knobs. Every control has Info View text (`help()` puts a knob cell's text on its Knob); named `samplerMode`, `samplerStrip`, `samplerWave`, `samplerBpm`, `samplerHalf`, `samplerDouble`, `samplerWarp`, `samplerWarpMode`, `samplerSliceBy`, `samplerPlayback`, `samplerTrigger`, `samplerLoop`, `samplerFilterOn`, `samplerLfoOn` for the tests. Drops of audio files still land through the `DevicePanel` (`MainWindow::loadIntoSampler`). Captures in `docs/screenshots/sampler/`.
7. **Tests (38 new; 1062 in all)**: M32 — the spec's indices; slicing the bursts file by transient (4), beat (8), region (6), manual (3), within Start .. End, at the lowest Sensitivity; Slice Poly / Thru / Mono and the playhead scope, a key past the last slice; 1-Shot Trigger vs Gate and Fade In; Classic Loop Start, the crossfade, Voices and Retrig; Warp Re-Pitch (an octave up, half the time), Tones and Complex (half the time at pitch), Warp off; the filter's LP / Off / HP and the LFO onto pitch and volume; loading finds 120 BPM for a two-bar loop and clears the old manual slices; slices to a clip (16 notes from C1, two bars); the panel's named controls and height, the mode buttons and the strip, dragging the ▶ flag (one undo step), a press away from the flags, double-click adds and removes a manual slice, `:2` / `x2`, Warp, and undoing them. M4 sets: the Sampler's tempo, slices and mode round-trip.

**Next AI:** `sampler::` is the parameter map; append, never reorder. A new slicing rule goes into `sampler::slices()` (no allocation) and a `SliceBy` choice; a new warp path into `SamplerVoice::render` beside the Stretcher. The Sampler ignores pitch bend / wheel / pressure like the other small instruments (Milestone 17's gap stands). Not done: a per-slice nudge / gain, pads-style slice preview on click, and Slice to a new MIDI track in one command (drag the band instead).
## Main integration · 2026-10-18

User explicitly requested pushing the Osmium work to main. Fetched origin; main was clean at `ec504a7` (0.31.0), so `osmium-usability` (`a53a78b`, 0.31.1) fast-forwarded without conflicts or code changes. The sampler branch/worktree was not merged or modified. The previously recorded five passing suites (including 70 Osmium checks) validate this exact application code; this integration only updates documentation. Stack remains native C++17 / Qt 6 Widgets, Project/EditScope undo, DeviceSpecs/SetFile persistence and shared Monolith/Osmium rendering.

Built: enlarged controls, theme-accent dual rings, 257-node curve editing, precise/group manipulation, zoom and responsive canvases described below. Required work is complete; push this integration to origin/main and verify synchronization. Future sampler integration is separate: preserve its device changes, Osmium's curve stride/storage, both handoffs and the newer release version.

## Osmium usability · complete · 0.31.1 · 2026-10-18

Isolated branch **`osmium-usability`**, worktree **`lattice-osmium`**, based on 0.31.0 (`ec504a7`). Claude owns the separate `sampler` worktree; it was not modified or merged. This branch uses **0.31.1** to avoid colliding with the sampler's **0.32.0**. Keep the newer version when integrating both branches later.

1. **Knobs:** all Osmium dials, including the compact rack, have a minimum 64px dial / 34px face. Macros use 84px dials / 54px faces. The inner ring/base disk and outer modulation annulus have separate hit areas, with an inert gap. Labels cannot accidentally edit either value. A full faded accent track remains visible when unassigned and behind assigned ranges. Bright portions, source slots, range inspector and filter previews use the current theme accent. Existing source/page behavior, mapping targets and parameter indices retained.
2. **Layouts:** Sound sections reflow into five, three or two columns without shrinking knobs. Smaller panes scroll. Envelope, LFO, step and curve canvases expand vertically. Existing integrated DeviceView host remains; editor minimum 1060×620, with scroll access below that. Dark, Light, 1920px and narrow 1060px captures reviewed in docs/screenshots/osmium-usability.
3. **Curve editor:** 1/4–1/256 time grids, independent Y snap, 1–8× zoom/pan, 257-node freehand with interpolation across skipped positions. X / Y / outgoing Bend fields accept exact percentages; segment shape follows the selected segment. Shift-click and marquee multi-selection, preserved group spacing/level offsets, Ctrl axis constraint, fine keyboard nudges, Tab node cycling, Delete and fixed endpoint times. Off-center grabs retain their offset. Each graph, group, freehand or numeric drag is one undo step. Drawing resamples at the selected X resolution; Free uses 256 intervals.
4. **Storage/audio:** osmium::kCurvePoints = 257, while monolith::kCurvePoints stays 32. SetFile preserves dense curves only for Osmium. Context::curveStride defaults to legacy 32; Engine opts into preallocated 257-slot buffers for both curves and passes the explicit stride. Shared evaluator uses binary search, preserving segment behavior without linear work per dense point. No audio-thread allocations added. Builds without this branch truncate curves above 32 nodes; retain this compatibility note when distributing dense sets.
5. **Verification:** all five CTest suites passed in **66.03s**, no reruns or weakened checks: **1024 smoke, 70 Osmium, 97 Drum Buss, 954 Drum Studio, 694 Osmium bank**. Osmium checks cover knob sizes, inner/outer/gap hits, custom accent and faded-track pixels, exact fields, 257-node save/load, legacy limit, dense stroke/group undo, zoom/pan endpoints, independent Y snap, marquee, axis constraints, select-all and off-center grabs. A real Engine test proves that nodes beyond 32 on Curve 2 reach audio with the correct stride. Final screenshot/export run also passed 70/70. Logs: /tmp/osmium-usability-ctest.log, /tmp/osmium-usability-final-ui.log, /tmp/osmium-usability-final-build.log.
6. **Deliverables:** README, docs/osmium-editing.md, manual checklist and seven reviewed screenshots. Installer: `dist/sessions_0.31.1+20261018203142_amd64.deb` (~40MB, ignored). Extracted application reports Sessions 0.31.1; the focused executable copied beside it also passed 70/70 from the installed directory (/tmp/osmium-usability-installed.log).

**Stack / next AI:** native C++17 / Qt 6 Widgets; OsmiumBody.cpp owns controls/plots/editor, DeviceBody/Project/EditScope own parameter and undo behavior, DeviceSpecs/SetFile own saved limits, Engine/Context and shared Monolith.cpp own rendering. Required implementation is complete. The Osmium work was subsequently integrated into main at the user’s request (see above). Next logical step is review/integration with the sampler branch when the user requests it: preserve the sampler device additions, explicit curve stride, latest release version and each branch's handoff notes. Do not merge automatically. Optional hardware audition and controller checks are in docs/test-checklist.md. No user work was reverted.

## Milestone 31 · Drum Buss · 2026-10-18

Complete, Sessions **0.31.0**, based on clean `9364627` (0.30.0). User requested a native Drum Buss and supplied a compact Ableton reference screenshot. The new panel follows its three control columns and Bass/Out meter arrangement, using Sessions' device colors and standard 192px height. No other device layout/parameter indices changed.

1. **Registered:** DeviceType::DrumBuss appended after Osmium, 14 stable parameters in drumbuss:: (Trim, Comp, Drive, Type, Crunch, Damp, Transients, Boom, Frequency, Decay, ForceNote, BassSolo, Output, Mix). DeviceSpecs registers Browser → Audio Effects → Drum Buss; createEffect delegates to createDrumBuss. All controls use DeviceBody/Project for undo, automation and MIDI mapping. Sets and .sessionspreset files round-trip all controls. Native device count is now 51.
2. **Audio:** `audio/DrumBuss.cpp` is Sessions' own implementation, informed by the official Ableton manual, not bit-identical emulation. Fixed linked compressor (3:1, -18dB, 6dB knee, 2ms/85ms, +3dB makeup); Soft waveshaping, Medium linked limiting, Hard clipping/bass emphasis; sine-shaped mid-high Crunch, smoothed Damp coefficients, high-band transient shaping above 100Hz (positive adds attack/sustain, negative tightens tails). Four-times oversampling uses shared 64-tap FIRs; the original-input branch is phase matched through resampling and a Linkwitz-Riley bass crossover for parallel mix. This adds about 16 samples fixed delay, plus crossover phase; the host has no track delay compensation. Bass Decay gates the incoming low band independently of Boom. A mono damped complex resonator, excited by low-band changes, adds Boom; Frequency 30–120Hz, Decay T60 40ms–2s, Force To Note quantizes to equal temperament. No sound from silence, bounded resonant excitation, smoothed modes/amounts/tuning and allocation-free process/panic.
3. **Panel:** `EffectBodies.cpp` adds DrumBussBody and DrumBussMeters. Three character buttons, all knobs/switches, exact real-unit Trim/Output/Mix fields, effective pitch/frequency readout, Bass Solo, peak Bass/Out meters and Comp GR. The native panel/title/background are captured for opaque screenshots. Dark/Light reviewed; fields accept 6 as +6dB and 50 as 50%.
4. **Verified:** 97 standard focused checks pass; exporter adds six audition/WAV checks (**103/103**). Tests measure harmonics, reduced clipping aliases, linked dynamics, positive/negative transient sustain, low-band preservation, incoming/Boom decay, actual forced-note tail (58.270Hz), silence/panic, stereo separation, mixed-path average (error 6e-8), parameter automation/mode changes, 32/44.1/48/96kHz, randomized controls, serialization/presets and actual Qt controls/typing. First Decay test caught cancellation from subtracting a phase-shifted low-pass copy; corrected with a proper matched crossover. Existing smoke now covers 51 devices, all controls/fuzz/audio limits; Drum Buss rendered one second in 27.7ms on this host.
5. **Full regression:** all five suites passed: 1024 smoke, 44 Osmium, 97 Drum Buss, 954 Drum Studio, 694 Osmium bank. Initial full CTest hit two timing-sensitive ALSA auto-connect/theme screenshot assertions; unchanged isolated smoke rerun passed, with byte-identical before/after Dark captures, then CTest --rerun-failed passed in 23.37s. No assertions weakened. Other four suites passed in the initial run. Logs: /tmp/sessions31-ctest.log, /tmp/sessions31-ctest-rerun.log, /tmp/sessions31-smoke-rerun.log, /tmp/drum-buss-final-export.log. Installed-layout extraction reports Sessions 0.31.0 and passes 97/97 focused checks (/tmp/drum-buss-installed.log).
6. **Artifacts/docs:** `dist/sessions_0.31.0+20261018195528_amd64.deb` (~40MB) and `dist/Drum Buss - Before and After.mp3`. The 29.20-second shared-gain reel compares dry/processed recorded Rock, DnB, Techno, Metal and Reggae; source peak .580, no clipped samples, timing guide in docs/drum-buss-audition.md. README, docs/drum-buss.md and manual checklist updated; three reviewed captures in docs/screenshots/drum-buss. Existing .30 clip/preset archives remain in ignored dist; automatic packaging keeps only the latest .deb.

**Stack / next AI:** native C++17 / Qt 6 Widgets, DeviceSpecs/Model for data, DeviceKit/DeviceProcessor for audio, DeviceBody/EffectBodies for compact controls. Required work is complete. Optional next steps: audition on hardware across buffers and match loudness for the user's drum material; add host-wide delay compensation if return-path phase matching across different tracks is needed (beyond this device's internally aligned Dry/Wet). Keep the compact Ableton-like style, existing theme colors and stable indices. No user work was reverted.

## Milestone 30 · Recorded Drum Studio and integrated editors · 2026-10-18

Complete, Sessions **0.30.0**. The previous Osmium preset/page-fix release 0.29.0 was committed and pushed as `69f1036` before starting this work. User's latest design direction is Ableton-like compact functional devices using Sessions theme colors; this overrides the earlier Material 3 direction for these devices. No user work was reverted.

1. **Recorded kits:** `audio/DrumStudio.cpp` and `DrumSamples.cpp/.h` replace synthetic acoustic drums with 285 unmodified mono PCM recordings (66.6MB), five velocity layers for 19 articulations in three AVL kits. Six recipe choices preserved: Black Pearl for Studio Rock/Tight Pop, Red Zeppelin for Vintage/Metal/Big Room, Blonde Bop for Jazz. 64 overlapping voices, per-articulation GM map, pitched high tom, output-rate conversion, hat choking, damping/sustain, per-drum controls, eight mutes/eight solos and channel meter scopes. All original 50 indices retained; switches appended (66 total). Shared recordings prepared outside audio rendering; panic clears preallocated buffers. Extra overhead/room/bleed are modeled ambience, not separate recorded mics. Velocity layers are not round robins.
2. **Assets/licensing:** `assets/drums` is the renamed Sessions Acoustic Collection, CC-BY-SA 3.0 with Glen MacArthur's produced-music exception. WAVs remain unchanged; complete author/license notices/PDF/legal code ship beside them. This separate sample license does not change the code license. `scripts/fetch-drum-samples.py` reproduces the selected/remapped collection from checksum-pinned source archives. Normal builds/playback require no network/plugin/library download. CMake installs `share/sessions/drums` and tracks asset changes for packaging; build-tree runs fall back to source assets. About 134MB shared decoded sample memory.
3. **Grooves:** 92 one/two-bar original programmed patterns in 19 categories. Added Techno, Breakbeat, Disco, Afrobeat, Dubstep plus DnB, Metal, Reggae and fill variations, preserving existing groove indices. Search/style filter, velocity/timing humanization, loop preview without transport/set edits, grid/playhead, Session/Arrangement drag/drop and To Clip. `grooves::midi/exportMidi` writes SMF type 0, channel 10, 960 PPQN, tempo, 4/4 and exact loop end. Optional test exporter writes all .mid/.sessionsclip files and a 19-category audition reel.
4. **Integrated UI:** DeviceView owns an on-demand expanded DeviceBody independent of the compact rack's rebuild lifecycle. Expand replaces the chain in the main detail pane; Device chain collapses it; MainWindow allows a taller resizable pane. Track changes hide/restore the current editor, device deletion releases it, undo/theme/preset changes preserve it. Osmium's QDialog removed; Sound / Modulators / Voice & mixer / Effects pages now fit a 500px minimum body, with scroll access on smaller windows. Green modulation ranges and the 0.29 page fix remain. Drum Studio has Kit (articulations, waveform, velocity), Mixer (8 faders, mute/solo, meters, ambience) and Grooves pages. Neutral Sessions surfaces/accent with compact squared controls; Light theme's persistent groove list explicitly refreshes its local stylesheet. Reference inspected: FLOW Orbit in `/home/ozzy/Documents/creative-projects/daw/src/StudioWorkflows.cpp`.
5. **Verified:** full CTest 4/4 passed in **63.94s** (1024 smoke, 44 Osmium gestures/persistence/audio, 954 Drum Studio checks, 694 Osmium bank checks). Focused exporter passed **1232 checks**, including all 92 native-clip and MIDI exports. All 22 GM pitches × six recipes and every groove render finite/audible below full scale. Dense Metal Descent initially exceeded full scale; common kit gain reduced 25%, worst default groove peak now **0.841**. Drum audition: **56.417s**, peak **0.528**, no clipped samples. Installed-layout check extracted the .deb, byte-compared all 285 files/licenses and reran 954 checks from its usr/bin; strace confirmed all 285 WAVs opened from installed share/sessions/drums and none from the checkout. Dark/Light and full-window captures reviewed; actual main-window splitter and undo shortcut tested. Audio checks do not replace subjective hardware listening.
6. **Release artifacts:** ignored `dist/sessions_0.30.0+20261018125318_amd64.deb` (~40MB), `Drum-Studio-92-Grooves.zip` (92 MIDI + 92 native clips, guide and credits), `Drum Studio - Audition.mp3`. Previous 40-preset ZIP and Osmium MP3 remain in dist; automatic packaging keeps only the newest .deb. Docs: README, test checklist and docs/drum-studio.md; new shots under docs/screenshots/drum-studio, refreshed Osmium shots. Logs: /tmp/sessions30-ctest.log, /tmp/drum-studio-export-final.log, /tmp/sessions30-installed-test.log.

**Stack / next AI:** native C++17 / Qt 6 Widgets; Project/EditScope for undo and mapping, DeviceSpecs/Model for device data, DeviceBody for control/automation refresh, DeviceView/MainWindow for the embedded host, Grooves for notes/clip/MIDI conversion. No required implementation remains. Optional follow-up order: (1) audition/tune the kit recipes and Osmium bank on the user's monitors/controllers, (2) add genuine multi-take round robins or separately recorded mic channels if a larger licensed library is desired, (3) retain multiple devices' embedded editor state concurrently (the host currently retains the current editor across track changes; opening another replaces it). Do not reintroduce floating windows or change saved parameter indices.

## Milestone 29 · Osmium DnB bank and page stability · 2026-10-18

Complete, Sessions **0.29.0**, based on clean 0.28.0 (`292f3e1`). Claude's endless DragValue gestures, numeric editors and dialog sizing are preserved.

1. Fixed Voice & mixer / Effects returning to Modulators during base-value or modulation-ring gestures. Source focus and explicit navigation are separate; theme refresh preserves the page. Explicit source-chip navigation still opens its editor.
2. Added 40 playable factory sounds: eight each Pad, Lead, Sub, Reese and Wobble. `core/OsmiumPresets.h/.cpp` owns complete normalized parameters, curves and routes, shared by Browser Sounds and the searchable Osmium selector. Eight consistently named macros: Tone, Warp, Drive, Width, Space, Echo, Body, Air. Factory identity follows persistence and undo/redo. Original presets remain; no DSP/source/parameter indices changed.
3. Verification complete: CTest 3/3 passed in 61.98 seconds: 1024 smoke checks, 44 Osmium UI/gesture checks and 694 bank checks. Every patch rendered at 170 and 174 BPM, pads as chords, each macro separately and all eight at maximum; finite audible output, no clipping, centered default subs and portable preset round trips checked. Shared audition reel is 116.8 seconds, with no clipped samples. Automated checks do not replace listening on hardware.
4. Updated README, manual checklist, `docs/osmium-dnb-presets.md`, measured CSV and two reviewed screenshots. Deliverables in ignored `dist/`: `sessions_0.29.0+20261018122802_amd64.deb`, `Osmium-DnB-40-Presets.zip` (40 portable presets, guide, levels), `Osmium DnB - Audition.mp3` (common +8 dB preview gain). Test exporter can regenerate WAV/presets/guide/levels.

Stack: native C++17 / Qt 6 Widgets, DeviceSpecs + Model for factory Sounds, OsmiumBody for its editor, Monolith.cpp for shared DSP. No required work remains for this milestone. Next authorized work: recorded acoustic Drum Studio kits, expanded multi-style MIDI grooves, integrated larger Osmium/Drum Studio editors using FLOW Orbit as a reference, and Ableton-like devices using Sessions theme colors. Read the next in-progress section when present.

## Milestone 27 · Osmium instrument · 2026-10-18

**Last updated:** 2026-10-18 · **Milestone 28 done (an endless drag for every value field: at the screen's edge the pointer jumps to the other edge and the drag goes on; typing into a field is opaque and reads as the field does, 35 for 35 %; dialog buttons fit their text; the export Tail is a drag field)**, after **Milestone 27** (Osmium, Codex) · v0.28.0 · 50k lines · builds clean on Qt 6.4, all 1024 smoke checks pass, plus the 33 Osmium checks · ships a .deb on every build · `docs/test-checklist.md` lists what to try by hand.

1. Inspect existing Monolith, device registration, undo, persistence and panel constraints (done). Monolith has 170 parameters, 26 sources, two 32-point linear curves; rack bodies are 192 px tall.
2. Core complete: Osmium registered separately with 182 parameters. Monolith retains 170. Shared wavetable DSP now has an Osmium-only timed ADSR path with independent segment bends. CurvePoint stores optional outgoing bend and interpolation (curve / smooth / hold), read/written by SetFile for sets and presets; two-coordinate legacy points stay linear.
3. Editor complete in src/ui/devices/OsmiumBody.cpp: compact rack plus 1180×806 floating workspace, Material 3 surfaces with semantic theme entries, three oscillators, two filter responses, source chips, direct green range rings, signed precise percent entry and reachable values in real units, independent envelope diamonds, shaped curves / freehand / snap / copy / invert / reverse, LFOs, steppers, all voice/FX controls, eight macros, five starting patches. Editor windows survive panel rebuilds by project/device identity.
4. Verification: the full existing smoke suite and 33 focused Osmium checks pass (CTest 2/2, 23.01 seconds). Added real Qt gesture coverage for ring/base separation, signed ranges and percent typing, same-device drops, node/diamond editing, freehand interpolation, one-step undo, set/preset persistence, legacy points, actual rendered attack/curve differences, editor survival across DeviceView rebuild/deferred deletion, actual Ctrl+Z/Ctrl+Shift+Z shortcuts, and automatic editor closure when the instrument is deleted. Dark and Light screenshots visually checked. New suite: tests/osmium.cpp / sessions_osmium; ctest runs both suites. Optional capture: QT_QPA_PLATFORM=offscreen ./build/sessions_osmium <directory>. README and docs/test-checklist.md describe operation. Final release package: `dist/sessions_0.27.0+20261018114443_amd64.deb`. Eight reviewed editor/rack captures are saved under `docs/screenshots/osmium/`; README shows the curve editor.

**Architecture / next AI:** The stack is native C++17 / Qt 6 Widgets, no web UI or plugin dependencies. `osmium::` in DeviceSpecs.h appends 12 bend knobs after Monolith's original 170. `createOsmium` in Monolith.cpp selects the separate timed segment envelope path; Monolith's ADSR path stays unchanged. `core/ModulationCurve.h` is the single evaluator for curve drawing and audio. `CurvePoint::bend` and `shape` describe the outgoing segment; SetFile writes optional third/fourth coordinates only when needed, and preserves old two-coordinate data. Existing render snapshots already copy CurvePoint by value, so no audio-thread allocations were added. All edits use Project scopes, automation touches and deviceChanged; modulation uses a separate merge-key domain to avoid colliding with base-parameter edits.

`OsmiumBody.cpp` owns the compact panel, direct range controls, source buttons, plots and full editor. All 182 parameters expose mapping targets. Choices and pitch fields accept source drops and reveal their range in the inspector. Envelope Level Curve shapes both audio modulation and its displayed contour in Osmium; Monolith retains its previous behavior. Floating windows are reused by Project pointer + stable device ID; a panel hands the window to its replacement or detaches it until the next panel, and Project destruction frees it. Editor Ctrl+Z/redo shortcuts use the shared undo stack, defer while a gesture is active, and accept both Ctrl+Shift+Z and Ctrl+Y for redo. The editor follows stable track identity; a 33 ms visible timer follows automation and live meters and closes when its device disappears. Theme colours live in Theme.h; selected source/primary text handles both dark and light colour contrast.

**Remaining optional work, in logical order:**
1. Listen/tune the five patches with a hardware controller and different audio buffers (manual checklist). Automated audio checks verify finite output and audible behavior, not subjective sound quality.
2. If extending envelopes, add a separate MSEG mode rather than changing the current saved ADSR meaning. Current envelopes are ADSR with independent continuous segment bends; phrase curves are two 32-point sources.
3. If a responsive editor is desired, replace its fixed 1180×806 canvas and scroll container with adaptive card layouts; the current smaller-window behavior is scrolling.
4. Add more patches or a searchable patch browser if wanted. Current five starter patches reuse Monolith sound recipes with Osmium's envelope path; normal Sessions preset saving works for all curve and route data.

The M26 work that was already in the workspace was committed by another task while Osmium was underway, including early Osmium core scaffolding. Do not revert that commit or treat a small final diff as the whole Osmium change. No user work was reverted.

A clip-based DAW in C++17 / Qt 6 Widgets, built around a Session grid and an Arrangement timeline. This file is the running log between iterations: what exists, how it's built, and what to do next. Read it first, then `src/ui/MainWindow.cpp` for how the UI connects and `src/audio/Engine.h` for how sound gets made.

**Last updated:** 2026-10-18 · **Milestone 25 done (the app icon is yours to pick: six colour treatments in Preferences > Look, the window icon, a per-user desktop entry and the About logo follow; Preferences rebuilt as a sidebar of categories with pages of labelled rows; dialog buttons without icons, flat message-box glyphs, a menu pass)**, after **Milestone 24** (the logo, the About, launching after a stop) · v0.25.0 · 49k lines · builds clean on Qt 6.4, all 1012 smoke checks pass · ships a .deb on every build · `docs/test-checklist.md` lists what to try by hand.

---

## Project layout

```
sessions/
├── CMakeLists.txt            # sessions (app), sessions_core (static lib), sessions_smoke (test), deb (package)
├── cmake/MakeDeb.cmake       # stage install -> dpkg-shlibdeps -> dpkg-deb, into dist/
├── handoff.md                # this file
├── docs/test-checklist.md    # what to try by hand (hardware, ears): everything since M5
├── README.md                 # build, install, shortcuts
├── assets/
│   ├── icon/sessions.svg     # app icon (the mark in colour on a charcoal tile) + rendered PNGs (16–512); also the .sessions file icon
│   ├── icon/variants/        # the six colour treatments of the icon (A–F), all in the resources: Preferences > Look > App icon; sessions.svg is A
│   ├── logo/                 # the logo: sessions-logo.svg (charcoal), -light.svg (for dark pages), sessions-mark.svg (the S alone)
│   ├── sessions.desktop      # Exec=sessions %f, MimeType=application/x-sessions-set (also in the resources: the per-user override's template)
│   ├── sessions-mime.xml     # the .sessions MIME type (installed as /usr/share/mime/packages/sessions.xml)
│   └── resources.qrc         # compiled into sessions_core; main() calls Q_INIT_RESOURCE
├── docs/screenshots/         # offscreen captures of each view (--screenshots); devices/ has every device panel
├── src/
│   ├── main.cpp              # app setup, stylesheet, command line: [set], --screenshots, --render, --reopen
│   ├── core/
│   │   ├── Model.h/.cpp      # Project, Track (+ returns), Clip, Note, Device (+ rack chain zones), Envelope, Locator; track/scene/rack ops; undo; demo / empty set
│   │   ├── SetFile.h/.cpp    # .sessions sets: JSON writer and (lenient, sanitizing) reader
│   │   ├── Presets.h/.cpp    # your presets (.sessionspreset): save / read / list / rename, the User Library folder
│   │   ├── Sample.h/.cpp     # audio files: WAV / AIFF decoders, libsndfile via dlopen, cache, overviews, WAV writer
│   │   ├── Tempo.h/.cpp      # the tempo of an audio file (onset flux, autocovariance, a comb over its multiples), matching it to the set's
│   │   ├── Import.h/.cpp     # audio files into clips, slots, the timeline or a Sampler (drops, browser)
│   │   ├── Mapping.h/.cpp    # MIDI Map / Key Map: Mapping (source -> target string) and the Mapper that applies / learns
│   │   ├── ArrangementOps.*  # split, clear range, insert / delete / duplicate time, consolidate
│   │   ├── DeviceSpecs.h/.cpp# every device and preset: kind, DSP type, parameters (kind, range, unit, default, choices), help text
│   │   ├── Scales.h          # scale masks for note-editor highlighting and Fold
│   │   ├── Transport.h/.cpp  # UI side of playback: commands, launch modes, recording (loop passes as takes), mirrors engine state + meters
│   │   ├── Theme.h/.cpp      # every colour (one Dark / Mid / Light value each) + themes (a base, tinted, an accent, overrides; .sessionstheme files) + the Qt stylesheet
│   │   └── Grooves.h/.cpp    # the Drum Studio's groove library: patterns by style as sixteenth lanes, to notes, clips, drops
│   ├── audio/
│   │   ├── Engine.h/.cpp     # render state snapshots, command queue, note scheduling, mixer, sidechain keys + track order, comps, offline render
│   │   ├── Devices.h/.cpp    # DeviceProcessor (+ meters), the MIDI effects, createProcessor(), display math for the panels
│   │   ├── DeviceKit.h       # private: parameter helpers (pv / pc / pb), PolySynth voice scaffold, Smoothed gains
│   │   ├── Instruments.cpp   # Analog, FM (4-op), Wavetable, Sampler, Organ, Bass, Electric, drum kits
│   │   ├── Monolith.cpp      # the big synth: 3 warped wavetable oscillators, mod osc, 2 filters, 4 envs, 2 LFOs, 2 steppers, macros, routes
│   │   ├── DrumStudio.cpp    # recorded acoustic kit voices, per-drum mixing and modeled ambience (sample library in DrumSamples.cpp)
│   │   ├── Effects.cpp       # EQ, Compressor, Saturator, Auto Filter, Delay, Reverb, Chorus, Phaser, Utility, Auto Pan, Redux, Gate, Limiter; KeyDetector (sidechains)
│   │   ├── Wavetables.h/.cpp # the band-limited tables (built by inverse FFT, 10 mip levels): 8 families, the last four made for basses
│   │   ├── Guitar.cpp        # Tuner, Guitar Amp (heads, tone stack, cabinets), the pedals; 4x oversampled clipping
│   │   ├── Convolver.h/.cpp  # impulse responses: partitioned FFT convolution (the amp's Loaded IR, NAM's, the Cabinet's)
│   │   ├── Stretch.h/.cpp    # the warp modes that keep the pitch: Tones / Texture (grains, WSOLA), Complex / Complex Pro (a phase vocoder)
│   │   ├── Nam.h/.cpp        # Neural Amp Modeler: the .nam reader (WaveNet / LSTM / Linear / ConvNet), the networks, resamplers, a device's Runtime, the NAM device
│   │   ├── JackDriver.cpp    # PipeWire / JACK duplex driver (libjack loaded at run time), port lists
│   │   ├── Dsp.h             # oscillators, ADSR, SVF (+ response), biquads (+ response), delay line, Freeverb parts, shapers, LFO shapes
│   │   ├── Driver.h/.cpp     # PulseAudio (PipeWire) output thread (device, buffer); output list; silent fallback clock
│   │   ├── MidiInput.h/.cpp  # MIDI from keyboards / controllers through the ALSA sequencer
│   │   └── Spsc.h            # lock-free single-producer / single-consumer queue
│   ├── widgets/              # DragValue, Knob, LevelFader (meter + fader marker + dB scale), Fader / Meter
│   └── ui/                   # MainWindow, ControlBar, Browser, SessionView, MixerStrip, OverviewBar (the bottom strip: clip + chain in miniature), ThemeEditor, GridShade (beat shading),
│       │                     # ArrangementView, ClipView, PianoRoll, SampleEditor, TakesEditor (take lanes + comp), DeviceView,
│       │                     # InfoView, InlineEdit (in-place rename + the shared track menu),
│       │                     # MapOverlay (map modes), Preferences,
│       │                     # DeviceDrag (what a device drag carries, and dropping it anywhere)
│       └── devices/          # device panels: DeviceBody (framework, knobs, buttons, ADSR / filter displays),
│                             # EffectBodies, InstrumentBodies, MidiBodies, RackBodies (+ the zone editor), GuitarBodies (+ the Cabinet), DrumStudioBody (one body per device)
└── tests/smoke.cpp           # synthetic mouse/key events + engine renders, no audio device needed
```

## Milestone 1: UI foundation (done)

Everything you can click, drag or type in the basic clip-launching workflow: Session grid with quantized launching, Arrangement timeline, note editor with velocity lane, device chain, mixer, browser, Info View hover help, tap tempo. The interaction checklist from M1 still holds.

## Milestone 2: sound (done)

### Fixes and requests from the M1 review

- **Tab / Shift+Tab never worked.** Qt's focus chain consumed Tab inside whichever child had focus, so `MainWindow::keyPressEvent` never saw it. Now `MainWindow::eventFilter` (installed app-wide) catches Tab / Backtab before focus navigation. Covered by the smoke test, which presses them with a child focused.
- **F11 toggles full screen** (View menu; XOR on `windowState()`, so leaving restores maximized).
- **Renamed to Sessions**: binary, targets, namespace (`sessions::`), desktop file, icon names, MIME type.
- **The window icon never loaded** (resources in a static lib need `Q_INIT_RESOURCE`). Fixed in `main.cpp`.
- **A .deb on every build.** The `deb` target is part of `ALL` and re-runs only when the app binary or assets change. The version is `0.2.0+<UTC timestamp>`, so every package installs as an upgrade. Depends come from `dpkg-shlibdeps`. Only the newest .deb stays in `dist/`.
- A small menu bar: File (Quit), Edit (Undo / Redo), View (view toggles, Full Screen).

### Audio engine (`src/audio/`)

**Threads.** The UI thread owns `Project`. On any change (`structureChanged`, `clipChanged`, `mixerChanged`, `deviceChanged`, `tempoChanged`), `Engine::publish()` builds an immutable `RenderState`. Publishes are coalesced per event-loop turn. The audio thread picks the newest state up through an `std::atomic<RenderState*>` and hands the old one back through an SPSC queue, so it's freed on the UI thread. Transport actions are `Command`s through a second SPSC queue. `process()` never locks or allocates.

**DSP state survives re-publishing.** Each `Device` has a stable `id`. `Engine` keeps one `DeviceProcessor` per id (created on the UI thread) and reuses it in every new snapshot, so moving a knob or undoing doesn't cut reverb tails or voices.

**Clock and launching.** The audio thread owns the playhead and the session state (`playingSlot`, `slotStart`, queued launch). Launches quantize against the engine's clock. A queued switch splits the block at the exact sample. A late launch keeps its phase. The arrangement loop wraps sample-accurately, and session clips keep their phase across the wrap. `Transport` sets its own state optimistically on a click, then mirrors the engine's state once `doneSeq()` has caught up with every command it sent. The displayed playhead is compensated for output latency.

**Note scheduling.** For each ≤128-frame block, each track's events come from either its playing Session clip or, if not overridden, every Arrangement placement under the playhead (looped by `Clip::length`, cut at the placement end). Events carry their length in samples, so there's no note-off bookkeeping; clip switches, stops, locates and loop wraps send `allOff`. `Note::chance` is honoured. A note on beat 2 at 120 BPM sounds on exactly sample 24000 (tested).

**Chain per track.** MIDI effects → instrument → audio effects (disabled devices are bypassed; a disabled instrument goes silent). Then activator / solo gate, fader (`(v/0.75)²`, matching `dbText`), balance pan, sends. Then the returns (audio effects only), the main fader, the metronome (at the Cue knob's level, after the main fader), and a soft clipper above −0.9 dBFS. Gains are smoothed over ~10 ms.

**Instruments.** Drum Kit / 808 Kit / Acoustic Kit (synthesized GM map: kick, snare, clap, rim, hats with choke, cymbals, tuned toms), Analog Synth (two detuned polyBLEP saws → resonant SVF with filter envelope), FM Synth (2-op, decaying index), Wavetable Synth (sine→tri→saw→square morph, 3-voice stereo unison), Sampler (placeholder tone until M4). Browser "Sounds" are presets: Warm Pad, Glass Keys, Sub Bass, Pluck Lead.

**Effects.** EQ (shelf / peak / shelf), Compressor (soft knee, partial auto make-up), Saturator, Auto Filter (LFO locked to one bar), Delay (tempo-synced ping-pong, filtered feedback), Reverb (Freeverb). MIDI: Chord (three shifts + velocity), Arpeggiator (rate, gate, octaves, Up / Down / Up-Down / Random / Played).

**Knobs mean something.** `DeviceSpecs` maps each knob to real units, and the Device View shows the value under each knob (e.g. "379 ms", "−14.0 dB", "2.9 : 1"). Delete resets a knob to its device default, not 0.5.

**Output.** `Driver.cpp`: `pa_simple` at 48 kHz float, ~30 ms buffer, 256-frame writes, on its own thread; latency is polled for the playhead. With no server, under `QT_QPA_PLATFORM=offscreen`, or with `SESSIONS_AUDIO=none`, a silent clock thread keeps time. `SESSIONS_AUDIO=pulse` forces the server even offscreen. The control bar shows real CPU load, "No Audio" in red on failure (hover for the reason), or "Audio Off" when silent on purpose.

### Mixer, returns, undo

- **Returns**: `Project::returns` holds A Reverb and B Delay (100 % wet). Tracks and returns share one index space (`trackAt`, `trackCount`, `isReturn`). Returns appear as clip-less columns in Session, strips with a Pre/Post switch, and slim lanes under the Arrangement. They only accept audio effects.
- **Meters** show the post-fader peak: −60 dB at the bottom, 0 dB at 95 %, ~0.6 s fall.
- **Undo / redo** (`Project::beginEdit / endEdit`, `EditScope`): snapshots clip contents, slots, arrangement and device chains before and after an edit; no-op edits leave no step. Keyed edits less than 1 s apart merge, so a knob drag or a typed rename is one step. Restores reuse the live `Clip` objects by id (editors stay pointed at the right clip) and emit `restored()`, which views use to drop raw pointers and note indices. Mixer moves are deliberately **not** undoable yet (they aren't in the snapshot).
- **Note preview**: adding, clicking or dragging a note (or clicking a key) plays it through the track's instrument.

### Dev helpers

- `sessions --render out.wav [seconds]` renders the demo arrangement offline (~90× real time). Handy for checking levels; the demo peaks around −1.5 dBFS with no clipping.
- `sessions --screenshots DIR [--size WxH]` captures six views: Session, Device View, Arrangement, automation mode, and the Clip View's Notes and Launch tabs (offscreen-safe, audio off).
- `sessions_smoke` / `ctest`: UI interactions, Tab / F11, undo, returns, engine timing (sample-exact note, bar-quantized launch), mute / instrument-off silence, demo mix sanity (all tracks sound, return fed, no NaN, below full scale).

### Gaps left after M2 (see M3 for what's been closed since)

- Return tracks have no solo and no sends of their own.
- Metronome plays through the main output (there's no separate cue output).
- For lower latency, ask rtkit for real-time scheduling or move to PipeWire's native API / JACK.

---

## Milestone 3: editing depth (done)

### Fixes from the M2 review

- **The UI didn't use a maximized window.** Session columns now fill the width (up to 320 px), clip rows grow only while the mixer keeps its 262 px minimum, faders take all remaining height, Arrangement lanes share the height (52–120 px) and the song fits the width until you zoom yourself, and the note editor zooms each clip to fill its width. `sessions --size WxH --screenshots DIR` checks other sizes.
- **View → Zoom (100–200 %)** scales everything through `QT_SCALE_FACTOR`, read from QSettings in `main()` before QApplication exists, so it applies on restart (the menu offers one).

### Model

- Tracks have a stable `id`; sends are a list (one per return, up to 8). `insertTrack / insertReturn / duplicateTrack / deleteTrack / moveTrack / renameTrack / setTrackColor` and `insertScene / duplicateScene / deleteScene / renameScene / captureScene` are each one undo step.
- **Undo covers everything editable:** whole tracks (name, colour, volume, pan, sends, activator, pre/post, slots, arrangement, devices, automation), scenes, tempo, main volume, locators. Not undoable: solo, arm, cue volume, loop region, playback. Restore rebuilds track lists by id and keeps each surviving track's runtime state. Merge keys: `clipMergeKey`, `deviceMergeKey`, `mixerMergeKey` (track 0 = main / tempo), `recordMergeKey` (merges regardless of time, so one recording pass is one step).
- `Clip` gained launch settings (mode, legato, quantization, follow actions) and a scale (on, root, scale). `ArrangementClip` gained `offset` (clip time at its start), so a split's right half starts mid-clip.
- **Automation:** `Track::automation` holds `Envelope`s keyed by target string (`mixer:volume`, `mixer:pan`, `mixer:send:k`, `device:<id>:<param>`), values 0..1, linear between breakpoints. Touching an automated control calls `Project::touchTarget`, which adds it to `Track::overriddenTargets` (runtime); the engine skips overridden envelopes until `reEnableAutomation()` (orange button in the control bar). The demo's pad fades in over its first two bars.

### Engine

- Commands address tracks by id; `PlayState` remembers the playing / queued **clip id**. `adopt()` re-maps state onto a new snapshot by track id and re-finds clips by id, so inserting, moving and deleting tracks or scenes (or dragging a playing clip to another slot) never confuses what's playing. `inSync()` (snapshot generation) gates index-based feedback in Transport.
- Launch modes: Trigger, Gate (release stops), Toggle (second click stops), Repeat (retriggers on the clip's quantization grid while held). Per-clip quantization overrides the global one. Legato starts the new clip at the old one's loop position.
- Follow actions fire once per launch after the follow time (0 = one loop): None, Stop, Again, Previous, Next, First, Last, Any, Other, with a chance between A and B. Previous / Next / Any / Other look at every filled slot in the track (no limiting to groups between empty slots yet).
- Automation is evaluated at the playhead once per ≤128-frame block, for volume, pan, sends and every device knob (returns too).
- Live notes: `NoteOn` / `NoteOff` commands; instruments release a held pitch on `noteOff`; Chord shifts note-offs; Arpeggiator drops released keys.
- `Transport::send` calls `Engine::flushPublish()` first, so a command always sees the edit that came just before it (e.g. a clip created and launched in one click).

### Session View

- Drag clips between slots (Ctrl copies; a playing clip keeps playing), drag track titles to reorder. Empty slots show ■ stop buttons, or ● record buttons on armed tracks.
- Right-click menus for tracks (rename, colour, duplicate, delete, move, insert track / return), scenes (rename, insert, capture and insert, duplicate, delete) and clips. In-place renaming (`inlineEdit`). Unnamed scenes show their number.
- Mixer strips show one send knob per return (4 per row) and follow automation; every move is undoable.

### Arrangement View

- Top: locator strip (double-click adds, drag moves, click jumps, right-click deletes, double-click a locator to rename), a draggable loop brace (move, resize either end, drag outside it to draw a new loop), then the beat ruler.
- Time selection across lanes (drag empty space, Shift extends; a click sets the insert marker and, when stopped, the playhead). Ctrl+L loop, Ctrl+E split (at the selection edges, or at the playhead inside the selected clip), Ctrl+J consolidate, Delete clears, Ctrl+D duplicates the material after itself; Insert / Delete / Duplicate Time from the Arrangement menu (Ctrl+I inserts the selection's length, or a bar). Copy / paste clips at the insert marker. Drag headers to reorder tracks.
- Automation mode (A, or the header-corner button): clips dim, each lane shows one envelope (pick the parameter under the track name; ● marks automated ones), click adds a breakpoint, drag moves it (it can't pass its neighbours), double-click / Delete removes it, right-click clears the envelope. Unautomated parameters show their static value dashed.

### Clip View

- Three tabs. **Clip**: name, length, Loop on/off, grid, Duplicate Loop, Clear. **Launch**: mode, legato, quantization, follow action (time, A, chance, B). **Notes**: −12 / −1 / +1 / +12, :2 / x2 (whole clip also halves / doubles the loop), Reverse, Invert, Legato, Scale (root + 12 scales), Fold. Tools act on the selected notes, or the whole clip.
- The note editor maps rows through `m_rowPitches`, so Fold can show only in-scale rows (scale on) or only rows with notes; dragging notes vertically moves along those rows.

### Computer MIDI keyboard and recording

- **M** toggles it (also ⌨ in the control bar). A W S E D F T G Y H U J K O L P = C..D#, Z / X octave, C / V velocity. It plays armed tracks, or the selected track if none is armed; text fields keep their keys; notes are released when the window loses focus. While it's on, letter keys are notes, so A (automation) is unavailable.
- **Session record:** ● in an empty slot of an armed track creates and launches a clip; played notes are written relative to its launch; the clip grows a bar ahead of the playhead; clicking it again ends the take, rounded up to a bar, and it loops.
- **Arrangement record** (● in the control bar): launched Session clips are written to the timeline as copies (overwriting what was there), and keys played on armed tracks that aren't playing a clip become a new timeline clip. Takes in progress draw in red; each pass is one undo step. Loop wraps close the takes.

### Known gaps / rough edges

- No MIDI input from real devices, and no audio input.
- Clip envelopes and multi-clip editing in the note editor weren't built (they were on the M3 list).
- Group tracks and freeze aren't there yet.
- Capture and Insert Scene copies the playing clips but doesn't relaunch the new scene seamlessly.
- Arrangement record doesn't overdub into existing clips or punch in / out; record quantization isn't there.
- Follow actions have no "groups" (runs of filled slots), and there's no Follow Action "Jump".
- Mixer moves and device knobs are smoothed per block, not per sample.
- The audio thread runs at normal priority (fine at a 30 ms buffer).

## Milestone 4: audio clips and files (done)

### Fixes from the M3 review

- **Notes on the Arrangement loop's start never played after the first pass** (and the click skipped a beat there). At the loop end the engine carried the block's sub-sample overshoot into the next pass, so its first window started just after the loop start and `[from, to)` excluded anything sitting on it. At tempos like 176.78 that happened on every pass. The loop now restarts exactly on its start (`Engine::renderBlock`). Smoke test: a note on the loop start sounds on 7 of 7 passes (the old engine: 1 of 7).
- **Space** plays from the start marker and stops; pressed again it plays from the start marker again. Shift+Space plays on from where it stopped. The Play button does the same as Space.
- **Start marker** (the insert marker, `Transport::startMarker`): clicking an Arrangement clip puts it at the clip's start, clicking empty space or a time selection at its start, the ruler or a locator there (a playing song also jumps). Stopping twice resets it to 1.1.1. It's drawn as an outlined triangle in the ruler.
- **Session columns** have a fixed default width (110 px) instead of stretching to the window (and rows a fixed height; see the layout pass below). Drag the divider at a column's right edge to resize it (72–480 px); double-click the divider for the default. Widths are `Track::width` (runtime, not undoable), saved with the set, and count as a change. The mixer strips are placed by hand under their columns (`SessionView::placeStrips`).
- **The returns and the Main column stay at the right edge**; the free space between the last track and the returns says "Drop Files and Samples Here".
- The window starts with keyboard focus on the Session grid, not the browser's search field, so the first Space plays.

### Layout pass (after the M4 review)

The user compared screenshots with the classic clip-launching layout and asked for the same layout. Matched:

- **Session clip slots keep one size** at any window size: columns 110 px (drag a divider to change one), rows 20 px (`ClipGrid::kRowHeight`). A bigger window gives the mixer more height; with more scenes than fit, the grid scrolls on its own (`SessionView::m_gridScroll`) and the mixer stays in view. Strips are placed through the grid's on-screen position, so a scroll bar can't misalign them.
- **Mixer strips** (`MixerStrip`): Sends zig-zag in two columns under a "Sends" label (returns keep the space empty). The volume section has a left column (volume field, pan knob, a large activator with the track number, S, ●), and on the right a `LevelFader`: the meter, the fader as a ◁ marker on the meter's own dB scale, and the scale (0 … 60).
- **Main column:** scene numbers in a narrow column on the right of the scene names. The Main strip's Sends area holds each return's **Pre / Post** switch (they moved off the return strips), then the main volume field, Cue volume and meter.
- **Browser:** two panes, a sidebar (Categories; Places: Current Project when the set is saved, your folders, Add Folder…) and a content pane with a Name header. **View → Browser** (Ctrl+Alt+B) hides it.
- **Arrangement** (`ArrangementView`):
  - An **overview** strip on top: the whole song in miniature, the visible part outlined; click or drag it to move there.
  - A **time ruler** (m:ss) along the bottom; the old hint line is gone.
  - **Headers** (236 px): a name block in the track's colour (drag it to reorder; the automation chooser sits in it), then a mixer column. Row 1: activator / S / ● (returns: activator / Pre-Post). Row 2: volume and pan fields. Row 3: send fields. Fields drag like DragValues (Shift for fine), double-click resets, automated values show in red. `ArrangementView::Field` / `fieldRect` are public for tests.
  - A **Main lane** under the returns with the main volume field. Lanes are at least 56 px, returns and Main 40 px.
- **Clip View:** Clip, Launch and Notes / Sample are **boxes side by side** with title strips (no more tabs). The sample editor draws **stereo files as two lanes** (per-channel overviews in `SampleData`).
- **Control bar:** **Follow** on the left (the Arrangement pages on with the playhead while playing; on by default), and **loop start / loop length** fields around the loop switch.
- Not copied, because they'd do nothing here yet: an I/O routing section, Track Delay, Groove Pool, Collections, the session clip status row, the crossfader.
- `--screenshots` now writes `05-clip-notes` (the clip boxes), `06-arrangement-audio`, `07-sample-editor`, `08-sampler` (the old `06-clip-launch` is gone with the tabs).

### Audio tracks and clips

- `Track::type` (`TrackType::Midi` / `Audio`), in `EditState` and the set file. **Create → Insert Audio Track** (Ctrl+Shift+T) and the track menu. Audio tracks hold audio clips and only audio effects (`Project::acceptsDevice`). MIDI-only things skip them: session recording, keyboard recording, consolidate, the computer keyboard's targets, double-click to create a MIDI clip. Clips can't be moved, dragged or pasted onto the other kind of track.
- An **audio clip** is a `Clip` with `samplePath` (absolute) and `sample` (`SamplePtr`, null while the file is missing). Settings: `warpMode`, `warpMarkers`, `gain` (dB), `transpose` (semitones), `reverse`. Clip length, loop, launch settings and follow actions work as for MIDI clips.
- **New clips** (`Project::newAudioClip`): if a power-of-two number of bars (1–32) at 60–200 BPM is within 20 % of the file's length at the set's tempo, the clip is warped to exactly that and loops; anything else (a one-shot) plays once at its own speed. Markers start as `{0 → 0 s, length → duration}`.

### Warping

- **Warp markers** pin clip beats to seconds in the sample; between them the audio is stretched (`warpSecondsAt`, linear, the end segments extrapolate). It's shared by the model and the audio thread.
- **Re-Pitch** reads the sample at `secondsAt(beat)`: speed and pitch follow the tempo (transpose is disabled).
- **Beats** slices on a 1/16 grid. Each slice starts at its warped position and plays at its own speed (times the transpose ratio), so the pitch stays. Where slices don't line up (another tempo, transposed), a slice fades out over 4 ms before running into the next one's audio or ending, and fades in over 0.3 ms. When the tempo matches, nothing is faded.
- **Declicking:** 3 ms fade-out before a placement's end, a launch switch or stop (known ahead through `queuedAt`) and the loop point; 0.3 ms fade-in at starts, so downbeats keep their attack.
- **Reverse** plays a reversed copy (`samples::reversed`, cached). Toggling it in the Clip View mirrors the markers, so the clip keeps its beats.
- Interpolation is 4-point Catmull-Rom from the file's own sample rate.

### Engine

- Audio tracks reuse the MIDI scheduling (launch quantization, legato, follow actions, Arrangement placements and loops). `emitSource` collects `AudioSeg`s (clip, timeline origin, bounds) instead of note events; `renderAudio` renders them before the track's audio effects. `ClipData` carries the sample (reversed if needed), markers, mode, linear gain and the transpose ratio.
- **Sampler:** plays its sample from C3 (pitched by the note, plus Tune), Attack, sustains while held, Decay = release, Volume. Without a sample it's the old tone. The sample reaches the processor through `Context::sample` (`DeviceSlot::sample` keeps it alive in the render state).
- **Browser preview:** `Engine::previewSample` puts the sample into the render state with a serial; the audio thread plays it once on the main output at the Cue level.
- **Offline export:** `Engine::renderOffline(path, RenderOptions, progress)` on a fresh `Engine` (its own DSP state, so what you're listening to keeps playing). Normalize renders to memory first.

### Samples (`src/core/Sample.*`)

- Files are read whole into memory as float stereo at their own rate (mono is duplicated), with a min / max overview per 256 frames for drawing.
- Built-in decoders: WAV (8 / 16 / 24 / 32-bit PCM, 32 / 64-bit float, `WAVE_FORMAT_EXTENSIBLE`) and AIFF / AIFC (`NONE`, `twos`, `sowt`, `fl32`, `fl64`).
- Everything else goes through **libsndfile loaded with `dlopen("libsndfile.so.1")`**: FLAC, Ogg Vorbis, Opus, MP3 (libsndfile ≥ 1.1), and WAVs the built-in reader can't handle. It isn't a build dependency; the .deb `Recommends: libsndfile1`. Without it, those extensions aren't listed.
- A cache by absolute path holds weak references, and an entry only counts while the file's size and modification time match. Re-exporting over a file reloads it.
- `samples::WavWriter` writes 16 / 24-bit (TPDF dither) and 32-bit float WAVs, streaming.

### Bringing audio in

- **Browser:** below the built-in categories come your **sample folders**: *＋ Add Folder…*, kept in QSettings `browser/folders`. Right-click a folder to rescan or remove it. Files are listed recursively (up to 5000 per folder), and search covers them too. **▶ Preview** (on by default, remembered) plays a sample when you click it. Double-click a sample: onto the selected audio track (the selected slot, else the first empty one), or into the selected MIDI track's Sampler. Sample drags carry file URLs as well as the browser payload.
- **Drops** (browser or file manager; `Browser::audioFilesIn`):
  - Session View: on an audio track's slot, the clips go there and into the slots below, adding scenes when they run out. Anywhere else (a MIDI track, returns, the free space) they go onto a new audio track named after the first file.
  - Arrangement: on an audio lane, at the snapped beat (Alt: unsnapped), one after another over what was there. Anywhere else, a new audio track.
  - Device View: onto the Sampler (its panel shows the sample's name and waveform), or anywhere on a MIDI track's chain: into its Sampler, replacing the instrument if there's none.
  - A `.sessions` file dropped on the window opens it.
  - Each drop is one undo step. Files that can't be read are listed in a warning.
- All of it goes through `src/core/Import.*` (`audioClips`, `toSession`, `toArrangement`, `toSampler`).

### Clip View and Arrangement for audio

- For an audio clip the **Sample** box replaces Notes: file (red when missing), warp mode, original BPM (`:2` / `x2`; changing it rescales the markers and the clip length around the first marker), gain, transpose, reverse. Length takes quarter beats; Dupl. Loop, Clear and Grid are off.
- **SampleEditor** replaces the note editor. It has a beat ruler with the loop end (drag it for the clip's length) and the waveform drawn through the warp map (past the clip's end it's dimmed). Warp markers: double-click adds one on the grid (Alt: free), drag moves one between its neighbours (the audio stretches), double-click or Delete removes one (two always stay). Playhead included; a drag is one undo step.
- The **Arrangement** draws audio clips as waveforms, looping with the clip and starting at the placement's offset. Missing files say so.

### Export Audio and Collect All and Save

- **File → Export Audio…** (Ctrl+Shift+R): the whole song (1.1.1 to the end of the last clip) or the loop region, plus a tail (0–30 s, default 2), as 16 / 24-bit or 32-bit float WAV at 48 kHz. Normalize sets the peak to −0.1 dBFS. It runs with a progress dialog and can be cancelled. `MainWindow::exportAudio` is the dialog-free part.
- **File → Collect All and Save** copies every sample from outside the set's folder into `<set folder>/Samples/Imported/` (a different file with the same name gets " 2"), points the clips and Samplers at the copies (one undo step), then saves. An untitled set asks for a place first.
- **Missing samples:** opening a set whose files are gone lists them. Those clips stay in the set, silent, with their paths.

### Set file format 2

- `kFormatVersion` is 2 (version 1 files still open; a version 1 reader refuses version 2 with "saved by a newer version").
- Tracks get `"type": "midi" | "audio"`, and `"width"` when a Session column was resized.
- Audio clips get an `"audio"` object: `path`, `relativePath` (to the set's folder), `warpMode` (`"beats"` / `"repitch"`), `markers` (`[[beat, seconds], …]`, sorted, both rising), `gain`, `transpose`, `reverse`.
- A Sampler gets `"sample": {path, relativePath}`.
- Reading prefers the relative path when that file exists, so a set folder that moved, with its samples inside, still finds them. A missing file keeps its path with no audio.
- `setfile::Extras` gained `trackWidths`.

### Dev helpers and tests

- `--screenshots` also captures the audio views (with a generated drum loop); see the layout pass for the file names.
- Smoke test, M4 audio checks:
  - Decoding: float / 16 / 24 / 8-bit WAV, AIFF, junk, the cache, reversing, libsndfile when present.
  - Clip warping guesses.
  - Engine timing: bursts land on the exact samples at their own tempo; Re-Pitch and Beats at half tempo (spacing and burst length); transpose; gain; reverse; loops with their downbeat; session launch.
  - The Sampler at C3 / C4; the preview.
  - Set format 2 with a moved folder and a missing file.
  - Import, Ctrl+Shift+T, Session and Arrangement drops (and undo), sample editor markers, Add Folder, export (length, normalize), Collect All.
- Sanitizer runs: configure a second build directory with `-DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" -DSESSIONS_BUILD_DEB=OFF`, build `sessions_smoke` and run it with `ASAN_OPTIONS=detect_leaks=0` (add `:verify_asan_link_order=0` where the runtime complains about the library order). In that Debug build M15's WaveNet timing check misses its budget (~3.7 s against 3 s); everything else passes.

### Known gaps after M4

- No audio input, so arming an audio track does nothing yet.
- Beats mode slices on a fixed 1/16 grid (no transient detection, no slice-length setting); there's no Complex / Texture mode and no "Warp off".
- The 3 ms fade at a clip's loop point dips loops that are already seamless.
- Samples load synchronously on the UI thread and stay whole in memory (a long file stalls the UI briefly). No relinking of missing files beyond moving the set folder with them.
- Consolidate doesn't render audio clips; Session slots don't show waveforms.
- Export renders the Arrangement only, at 48 kHz, WAV only.
- The Sampler holds one sample with no zones, loop points or root-key setting.

### Project save / load

Work survives a restart. A set is one JSON `.sessions` file (format below).

- **File menu:** New Set (Ctrl+N: two empty MIDI tracks, the A Reverb / B Delay returns, eight scenes), Open… (Ctrl+O), Open Recent (last 10, in QSettings `files/recent`; moved files are greyed out; Clear Recent), Save (Ctrl+S), Save As… (Ctrl+Shift+S; adds `.sessions` if you leave it off). Dialogs start in the last folder used (`files/lastDir`), else ~/Music. Startup still loads the demo, titled "Demo".
- **Title:** `Name[*] - Sessions` with `setWindowModified`. The set counts as modified when the undo stack isn't clean (`QUndoStack::cleanChanged`) or when a saved setting that isn't undoable (loop region, global launch quantization, metronome, cue volume) differs from what was last saved or opened (`MainWindow::m_savedExtras`). Undoing back to the saved state clears the *.
- **"Save changes?"** (Save / Discard / Cancel) before quitting (`closeEvent`), New, Open and Open Recent.
- **Opening** parses the whole file first (`setfile::read`), so a broken file leaves the open set alone. Then `Transport::reset()` (stop, rewind, back to the Arrangement, drop takes in progress without writing them), `Project::loadEditState` (ids kept, id counters moved past the highest loaded id, runtime state and **undo history reset**), and the transport settings. The first clip is selected and the Arrangement fits the song again (`ArrangementView::fitSong`).
- **Command line / file manager:** `sessions song.sessions` opens a set (`QCommandLineParser`, with `--help` / `--version`). The desktop entry has `Exec=sessions %f` and `MimeType=application/x-sessions-set;`. The .deb installs `assets/sessions-mime.xml` as `/usr/share/mime/packages/sessions.xml` and the app icon as the `application-x-sessions-set` icon. dpkg's triggers (shared-mime-info, desktop-file-utils, hicolor) refresh the caches, so there are no maintainer scripts. `MakeDeb.cmake` refuses to package if the MIME file, the file icon or the desktop entry's `MimeType` line is missing, and runs `desktop-file-validate` when it's installed.
- **View → Zoom restart keeps the set:** it saves a snapshot to `$TMPDIR/sessions-restart-<pid>.sessions`, starts `sessions --reopen <snapshot> --reopen-name <name> [--reopen-path <path>] [--reopen-modified]` (hidden options), and quits without asking. The new instance opens the snapshot as the original set (same path, name and unsaved state) and deletes it. Only the undo history is lost.
- **Fixed on the way:** `~MainWindow` now disconnects the project, transport and undo stack from the window. The undo stack's destructor calls `clear()`, which emits `cleanChanged` after `~MainWindow` has run, and `updateTitle` then touched freed members (heap corruption, found with AddressSanitizer).
- Smoke test: a set with every feature (three returns, pre-fader return, alpha colour, Unicode names, disabled devices, MIDI effects, automation on volume / pan / sends / device knobs, a clip shared by a slot and the timeline, split placements with offsets, every launch / follow / scale setting, empty scene names, locators, loop region, quantization, metronome, cue volume) saves, loads into a fresh `Project` with `captureEditState() ==`, keeps its ids and clip sharing, re-saves byte-identically, and plays. Plus broken / foreign / newer files, a sparse hand-written file, and the window: title and *, Ctrl+S, New, Open, recent list, reopen after restart. The test keeps its settings in a temporary `XDG_CONFIG_HOME`.

Gaps left in save / load:

- No single instance: double-clicking a set while Sessions runs opens a second window (and a second audio stream).
- No autosave or crash recovery, and the undo history isn't saved.
- Startup always loads the demo; there's no default set / template (a *Save Current Set as Default*).
- `--render` still renders only the demo; it could take a set.
- The engine keeps DSP state per device id, so a reverb or delay tail from the old set can ring on briefly in a device with the same id in the newly opened one.

### Set file format (`.sessions`, format version 1; see "Set file format 2" above for audio)

UTF-8 JSON from `setfile::toJson` (`src/core/SetFile.cpp`). Indented, keys sorted (`QJsonObject`). Doubles are written in their shortest round-trip form, so a save → load gives back exactly the same values. Beats are the time unit; every 0..1 value is normalized exactly as in the model.

```jsonc
{
  "format": "Sessions Set",            // identifies a set
  "version": 1,                        // setfile::kFormatVersion; newer versions are refused with a message
  "createdBy": "Sessions 0.3.0",       // informational
  "tempo": 120, "mainVolume": 0.75, "cueVolume": 0.6,
  "transport": { "loop": false, "loopStart": 0, "loopLength": 16, "launchQuantization": 4, "metronome": false },
  "scenes": ["Intro", ""],             // "" = unnamed (the Session View shows its number)
  "locators": [{ "beat": 16, "name": "Verse" }],
  "tracks": [{
    "id": 1, "name": "Drums", "color": "#f2905e",    // "#aarrggbb" when not opaque
    "volume": 0.75, "pan": 0, "sends": [0.12, 0],    // one send per return
    "active": true, "preFader": false,
    "devices": [{ "id": 1, "name": "Drum Kit", "kind": "instrument",   // or "audioEffect", "midiEffect"
                  "enabled": true, "params": [{ "name": "Tune", "value": 0.5 }] }],
    "automation": [{ "target": "mixer:volume", "points": [[0, 0.35], [8, 0.75]] }],  // [beat, value]
    "slots": [1, null, null],                        // a clip id (or null) per scene
    "arrangement": [{ "start": 0, "length": 16, "offset": 0, "clip": 1 }]
  }],
  "returns": [ /* like tracks, without slots and arrangement; sends empty */ ],
  "clips": [{
    "id": 1, "name": "Beat A", "length": 4, "loop": true,
    "notes": [{ "pitch": 42, "start": 0, "length": 0.25, "velocity": 0.8, "chance": 1 }],
    "launch": { "mode": "trigger", "legato": false, "quantization": -1,     // gate / toggle / repeat; -1 = global
                "follow": { "enabled": false, "actionA": "next", "actionB": "none", "chanceA": 1, "time": 0 } },
    "scale": { "on": false, "root": 0, "name": "Major" }                    // a name from Scales.h
  }]
}
```

- **Clips are stored once** and referenced by id from slots and placements, so a clip that is both in a slot and on the timeline is still one shared `Clip` after loading. Only clips the set uses are written.
- **Ids are written as they are.** Automation targets (`device:<id>:<param>`), the engine's DSP state and playback all key on them. `Project::loadEditState` keeps them and sets `m_nextTrackId / m_nextDeviceId / m_nextClipId` past the highest one (device ids named by envelopes count too).
- **Enums are words** (`kKinds`, `kModes`, `kFollows` in SetFile.cpp); the scale is stored by name. Reordering an enum or `kScales` doesn't break old files, but renaming a word does.
- **The reader is lenient and sanitizes.** Missing keys take the model's defaults, unknown words their default, values are clamped to their ranges (pitch 0–127, 0..1 values, tempo 20–999). Slots are padded / cut to the scene count, sends to the return count. Non-positive or duplicate track and device ids get fresh numbers. References to missing clips are dropped, as are unused clips. A set always has at least one scene and one track. A known device missing knobs gets the spec's defaults; its kind always comes from `DeviceSpecs`.
- **Not saved:** undo history, solo, arm, playback and launch state, automation overrides, selection, which view is showing, Arrangement zoom, the computer keyboard.
- **Versioning:** adding a key with a sensible default needs no version bump (old readers ignore it). Bump `kFormatVersion` only when an older reader would misread the file, and keep reading the old versions.

## Milestone 5: playing live (done)

The old "polish" list was too long for one pass, so this milestone took the part that matters for playing live; the rest moved to Milestone 7 (Milestone 6 became the devices).

### Session View requests (from the M4 review)

- **A divider between the clips and the mixer** (`SessionView::m_split`, a QSplitter; 5 px handle styled as `QSplitter#sessionSplit`). Drag it and the clip area grows or shrinks; where you leave it is remembered (QSettings `session/gridHeight`). Until you drag it, the grid fits the scenes. A bigger window always gives the extra height to the mixer.
- **Empty cells fill the revealed space.** The grid now scrolls its scene rows itself (`ClipGrid::m_firstRow`, mouse wheel, a slim position bar on the right). The track header row stays at the top and the stop row at the bottom; between them, rows past the last scene are filler cells. Double-click one on a MIDI track (or use its menu) and the scenes down to it are added with a new clip, as one undo step. Dropping a sample on one puts it exactly there.
- **Track status clocks** in the stop row (`ClipGrid::trackStatus`): a playing session clip shows its pass number (looping clips), a clock that fills once per loop, and the loop's length in beats. A 4-beat clip goes round four times while a 16-beat clip goes round once, so you can see when each one restarts.

### MIDI input (`src/audio/MidiInput.*`)

- Through the **ALSA sequencer** (which PipeWire serves too); ALSA is optional at build time (`SESSIONS_HAVE_ALSA`, `libasound2-dev`). Sessions opens one input port, "Sessions: MIDI In", and connects every readable MIDI port. It listens to the system announce port, so keyboards plugged in later are connected too. Ports switched off in Preferences are kept in `midi/off` by name.
- Events are read on the UI thread through a QSocketNotifier and come out as signals (`noteOn`, `noteOff`, `controlChange`, `activity`). `inject()` sends the same signals without ALSA (tests).
- Notes play the armed MIDI tracks, or the selected one, exactly like the computer keyboard, so session and arrangement recording take them too. The **sustain pedal** (CC 64) holds notes. The control bar's MIDI light flashes on input.

### MIDI Map and Key Map (`src/core/Mapping.*`, `src/ui/MapOverlay.*`)

- **View → MIDI Map Mode** (Ctrl+M) / **Key Map Mode** (Ctrl+K), or the **MIDI** / **KEY** buttons at the right of the control bar. In a map mode a transparent overlay tints every mappable control; the selected one is outlined and mapped ones are labelled ("CC 21", "C1 ch10", "Q").
- Clicking a mappable control selects it instead of using it. `MainWindow::eventFilter` catches the press app-wide only on mappable targets, so view switches and the mode buttons still work. Then the next CC / note (MIDI Map) or key (Key Map) maps it. Delete removes the selected control's mappings; Esc leaves.
- **Targets** are strings that name tracks and devices by id (see the comment in `Mapping.h`): track volume / pan / sends / activator / solo / arm, device knobs and on/off, main and cue volume, tempo (CC → 60–187 BPM), clip slots, scenes, track stops, Stop All, and play / stop / record / loop / metronome. Widgets declare theirs with a `mapTarget` property; the grid's cells come from `ClipGrid::mapCells()` / `mapTargetAt()`.
- **Behaviour:** continuous targets follow a CC (a note on one sets it by velocity); buttons and launches fire on a note-on or when a CC rises past 64, once per press; a note-off or key release releases Gate clips. Each source drives one target, and a control has at most one MIDI and one key mapping. Mapped moves are ordinary undoable edits (merged per gesture) and override automation like a hand move. Key mappings take precedence over shortcuts and the computer MIDI keyboard (not while typing).
- Mappings live in `Project::mappings`. They're saved with the set (`"mappings": [{source: cc|note|key, channel, number, target}]`, no format bump: older readers ignore it) through `setfile::Extras`, so mapping counts as a change. They aren't undoable.

### Preferences, window state, startup, one instance, Help

- **Edit → Preferences…** (Ctrl+,), applied as you change it:
  - **Audio:** output device (the sound server's outputs, listed with libpulse's async API in `audio::listOutputDevices`) and buffer (10–100 ms). `Engine::setOutput` restarts the output thread; the render state and DSP survive.
  - **MIDI:** the input ports, checkable.
  - **Look:** zoom, with Restart Now.
  - **Startup:** open the demo, an empty set or the last set (`startup/open`).
- **Window state:** size / place / maximized, Session or Arrangement, Clip or Device View, the browser and the bottom panel's height are saved on close (`window/...`) and restored at start (not with `--size` or `--screenshots`).
- **One instance:** `main.cpp` listens on a QLocalServer (`sessions-$USER`). A second start sends `open\t<path>` (or `show`) and exits, so double-clicking a set opens it in the running window (after asking about unsaved changes). `--reopen` (Zoom restart), `--screenshots` and `SESSIONS_NEW_INSTANCE=1` start their own.
- **Help:** Keyboard Shortcuts (Ctrl+/) and About.

### Tests and dev helpers

- Smoke test, new checks:
  - The divider, filler cells (double-click, undo), wheel scrolling, the clocks' numbers.
  - A real note through ALSA from a second sequencer client (auto-connected).
  - MIDI notes recorded into a clip; the sustain pedal.
  - MIDI Map (click selects, CC maps and drives, undo); Key Map (slot + key); CC buttons on the rising edge; a pad launching a scene; mappings in the set file.
  - Preferences (buffer applied, MIDI ports listed); window state; startup with an empty set.
- `--screenshots` also writes `09-midi-map`, and `01-session-clip` now has a scene playing (the clocks).
- The one-instance hand-off was checked by hand with two processes (under a test `USER`, so it can't touch a real instance's socket).

### Known gaps after M5

- MIDI is read on the UI thread, so a busy UI can add a few ms of jitter; no MIDI output, clock or MPE.
- Pre / Post switches, the arrangement header fields and the clip editors aren't mappable yet; no relative (endless-encoder) CC mode or mapping ranges.
- No audio output device for the cue / preview; the device list needs a running sound server.

## Milestone 6: devices (done)

Asked for: more effects and instruments, "all with a unique device UI, stylized the way the classic DAWs do it".

### Parameters as data (`src/core/DeviceSpecs.*`)

- Every device lists its parameters as `ParamSpec`s: kind (`Linear`, `Exp`, `Int`, `Choice`, `Toggle`, `FaderDb`), range, unit (Hz, dB, ms, semitones, …), default (a knob position) and choice labels / values. `paramValue` / `paramText` / `choiceIndex` turn a knob position (0..1, what the model, the set file, automation and MIDI mapping store) into units and text. The panels, the engine and the set file all read the same table.
- **Compatibility rule:** a device's existing parameters keep their index and meaning; new ones are appended. A set saved with fewer parameters loads with defaults for the rest (`SetFile::readDevice` pads; the round-trip test checks a 1-knob Reverb).
- The engine passes up to `kMaxParams` (32) knobs per device; automation targets work on all of them. `deviceMergeKey` now has 6 bits for the parameter.
- **Presets** are spec copies with other defaults; the browser's Sounds list is every preset (14 now: Acid Line, Formant Choir, Glass Keys, Hollow Lead, Pluck Lead, Reese Bass, Rock Organ, Saw Lead, Square Bass, Sub Bass, Suitcase Keys, Tonewheel Jazz, Tubular Bells, Warm Pad, …). The browser's categories are built from the table, sorted.

### DSP (`src/audio/`)

- **Instruments:** Analog (2 oscillators × 4 waves, octave, detune, noise, LP / HP / BP with key tracking, filter + amp ADSR, glide), FM (4 sine operators, 7 algorithms, ratios, per-operator level / decay, feedback, velocity → modulation), Wavetable (4 band-limited tables × 8 frames scanned by Position, 1 / 3 / 5 unison voices spread in stereo, sub, filter that opens with the amp envelope), Sampler (start / end / loop, root note, filter, ADSR, velocity), Organ (9 drawbars, percussion, key click, scanner vibrato / chorus V1–C3, drive, a rotary speaker that ramps between Slow and Fast), Bass (monophonic, overlapping notes slide without retriggering, accent on velocity > 100, 2-pole × 2 resonant LP with envelope, sub, drive), Electric (FM tine piano, velocity bark, bell partial, key-scaled decay, stereo tremolo, drive), the three drum kits (now with Volume).
- **Audio effects:** EQ (low shelf, bell with Q, high shelf, 12 dB/oct low / high cut, all frequencies adjustable), Compressor (knee, makeup toggle, output, dry / wet, GR meter), Saturator (analog / soft sine / hard clip / fold / digital), Auto Filter (LP / HP / BP / notch, synced LFO with 6 shapes and 8 rates, envelope follower), Delay (L / R times or linked, ping-pong or stereo, low cut and filter in the feedback), Reverb (pre-delay, input cuts, width, freeze), and new: Chorus (chorus / ensemble / vibrato), Phaser (2–12 all-pass stages or a flanger), Utility, Auto Pan, Redux, Gate (hold, return, floor), Limiter (1.5 ms look-ahead).
- **MIDI effects:** Chord (5 shifts), Scale (root + 12 scales, nearest note, ties go down), Velocity (range, curve, random), Arpeggiator.
- `DeviceProcessor::meter[2]` (atomics) carry what panels show live: gain reduction, the cutoff an LFO has moved to, an LFO phase, the last note in / out, the drum pad that was hit, the rotary horn's angle. `Engine::deviceMeter(id, k)` reads them.
- `PolySynth` voices render into a scratch buffer; `post()` processes the whole sound (organ vibrato / rotary, piano tremolo) before Volume, which glides (`Smoothed`) so knob moves don't click.
- `Devices.h` also exports `eqResponseDb`, `velocityMap`, `scaleMap`: the panels draw with the processors' own math.

### Device panels (`src/ui/devices/`)

- `DeviceBody` is the framework: `pos / real / pick / on / text` read a parameter (following its envelope while automation plays), `edit(i, v)` writes one (one undo step per knob gesture, touches automation), `editMany` writes several from a display drag (one step per gesture). Building blocks: `knob()` (name above, value below, MIDI-mappable), `toggle()`, `choices()` (segmented buttons, text or icons: `waveIcon`, `filterIcon`, `lfoIcon`), `chooser()` (a dropdown field), `section()`. Every control carries a `mapTarget`, so MIDI / Key Map reach every parameter. Bodies refresh on `Project::deviceChanged` and tick at 30 Hz for meters.
- Displays (`Display` subclasses) are dark, framed and draggable: `AdsrDisplay` (three handles), `FilterDisplay` (cutoff / resonance dot on the response), plus per device: EQ curve with five numbered band dots (wheel = Q), compressor transfer curve with knee and ratio handles, the live input dot and a GR meter, saturator curve, filter response that moves with the LFO, LFO shapes with a playhead, delay echoes (L above, R below), reverb tail (pre-delay, early reflections, T60 in dB), chorus / phaser / auto-pan motion, Redux staircase, gate level history with threshold and return lines, limiter GR history, FM algorithm diagram (click to change), wavetable view (frame playing + the table in a row), Sampler waveform with start / end flags and loop bar, organ drawbars, rotary horn, bass filter sweep (accent dashed), electric piano decay across the keyboard and tremolo lamps, drum pads (click to hear, light up when hit), arpeggio pattern, chord sliders + keyboard, scale map grid, velocity curve.
- `DevicePanel` (in `DeviceView`) is the title bar (activator, name with the device's help in the Info View, delete) over the body; every device is `kBodyHeight` (192 px) tall, each as wide as its body. The chain scrolls sideways (the host is pinned to the chain's minimum size; it used to squeeze panels over each other).
- `--screenshots` writes `10-device-chain` and `devices/NN-name.png` for every device.

### Fixes found on the way

- **Device edits weren't undoable.** Knob turns, the activator and MIDI-mapped device knobs wrote through a `Device&` taken *before* `EditScope` captured the snapshot; Qt's containers share data until written, so the write landed in the snapshot too and undo saw no change. They now look the device up after the scope opens (see Conventions).
- **`DelayLine::read` could read one past its buffer** when a tiny negative position plus the length rounded up to exactly the length. ASan caught it through the reverb's new pre-delay (it made the demo's reverb hiss); fixed and tested.

### Tests

- Every device renders finite, bounded audio with its defaults and with 25 random knob settings; instruments make sound; each renders a second of audio in milliseconds (the heaviest is ~4 ms).
- Behaviour: compressor GR + meter, limiter ceiling, gate open / shut, Utility mono / mute, reverb freeze, EQ response, Scale / Velocity / Chord, Sampler loop and start, FM algorithms differ, wavetable normalisation and mip levels, Bass slide vs retrigger.
- Panels: every parameter of every device has a control with a map target; every device but Utility has a display; bodies fit the height; knob edit = one undo step; an EQ dot drag moves two parameters as one undo step and undoes; automation shows on the panel; choice buttons; outside changes refresh panels; a long chain lays out side by side.

### Known gaps after M6

- No presets you can save yourself, no racks / macros, no device drag-reordering, no folding or hot-swapping devices.
- No spectrum analyser in the EQ; the organ's percussion retriggers on every note (a real one is single-trigger); the Bass slides only between notes that overlap.
- Wavetable has four built-in tables; no user wavetables.

## Milestone 7: recording and mixing (done)

Scoped from the M6 "next" list: the recording and mixing half (audio input, group tracks, clip envelopes, Warp off / transient slicing, background loading). Racks, Drum Rack, multi-clip editing, Complex warp, a light theme and other-platform packaging moved to Milestone 8.

### Audio input and recording (`src/audio/Driver.*`, `Engine.*`, `src/core/Transport.*`)

- `InputDriver` / `makePulseInput()`: a PulseAudio record stream (pa_simple, float stereo at the engine's rate) on its own thread, pushing blocks through `Engine::pushInput` into `m_inRing` (an `SpscFloatRing`, `Spsc.h`). `listInputDevices()` lists the server's sources (monitors included). The input opens only while something wants it (`Engine::setInputWanted`: an armed audio track, a Monitor In track, or an audio recording); `Transport::updateInputWanted` decides. Under `QT_QPA_PLATFORM=offscreen` / `SESSIONS_AUDIO=none` the input is a "Test input" fed only by `pushInput` (the tests use this).
- The audio thread pulls one block per `process()` (`pullInput`): the ring first builds a small cushion (capture and playback run in step but not in phase); past ~85 ms it drops the oldest audio to keep latency down. `inputLatencySeconds()` = the capture stream's latency + what waits in the ring.
- Per track: `Track::monitor` (In / Auto / Off: Auto = while armed), `Track::inputChannel` (0 = 1/2 stereo, 1 / 2 = that input on both sides). Monitored input is added before the track's devices, so effects process it. `takeInputPeak(i)` feeds `Track::inputMeter`; an armed audio track's strip meter shows `max(meter, inputMeter)`.
- Recording: `Engine::startCapture` / `stopCapture` (Command::Capture) make the audio thread copy the raw input into `m_recRing` (2 M floats) from the block where capture starts, with `captureStartBeat()` saying which beat that block began on. `Transport`'s AudioTake drains the ring every tick into a 32-bit float WAV (`samples::WavWriter`) at `recordingPath()`: `Project::recordFolder` (MainWindow sets it to `<set folder>/Samples/Recorded` on save / open; unsaved sets use `~/Music/Sessions/Recorded`).
- Latency compensation: each take's WAV starts `AudioTake::comp` (output latency + input latency) later in the capture than its start beat, so what you played against the beat lands on it (`drainCapture` cuts the file from `startFrame` to `stopFrame`). Session takes are rounded to the bar they ended in (like MIDI recording). Two warp markers map the file 1:1 at the tempo it was recorded at. One undo removes a whole recording (`recordMergeKey`).
- Arrangement Record + an armed audio track records onto the timeline (`ensureCapture` starts the capture as playback starts, so the take isn't late by a timer tick).
- Preferences → Audio → Input device ("audio/input" in QSettings; applied at startup) with a status line.

### Group tracks (`Model.*`, engine, views)

- `TrackType::Group`; members carry `Track::groupId` (the group track's id) and sit right after it (the model keeps each group a contiguous block: `insertTrack` joins the group it lands in, `moveTrack` moves a group with its tracks and never into another group, `groupTracks` makes the group before the first picked track — after the old group if they were picked out of one — and drops groups left empty). Groups don't nest.
- `Project::groupTracks(indexes)`, `ungroup(groupIndex)`, `groupOf`, `membersOf`, `hiddenByFold`, `setFolded` (a view setting, not undoable, saved in the set as "folded"). `selectedTrackIds` is the multi-selection (Ctrl / Shift+click titles in the Session View and Arrangement); `MainWindow::selectTrack` resets it when you select a track outside it.
- Engine: groups are buses (`m_busL/R[kMaxGroups]`). `renderBlock` renders members into their group's bus, then each group bus through its own devices / fader / sends into Main, in two passes. Solo propagates both ways (soloing a member keeps its group audible; soloing a group plays its members).
- UI: Create → Group Tracks (Ctrl+G) / Ungroup Tracks (Ctrl+Shift+G), also in the track context menu. Session View: the group's slots launch / stop its tracks' clips in that scene, its stop button stops them; ▾ folds (folded members get zero-width columns). Arrangement: a group lane shows its tracks' clips as a summary; ▸ / ▾ on the title folds (members' lanes get zero height); a member's title has its group's colour down the left. Group strips have no Arm.

### Clip envelopes

- `Clip::envelopes` (same targets as track automation: mixer volume / pan / sends, `device:<id>:<param>`), positions in clip beats, looping with the clip. While a clip plays (slot or timeline), `Engine::clipOverrides` sets those parameters absolutely, over the track's value and its Arrangement automation.
- Clip View → Envelopes box: Mixer or a device, then the parameter (● marks those with an envelope); Show swaps the editor for `EnvelopeEditor` (click adds a breakpoint, drag moves, double-click / Delete removes; removing the last point removes the envelope); Clear deletes it.

### Warp off and transient slicing

- `samples::finish` runs `findTransients` (high-frequency-content onsets; each pinned to the attack's first sample) → `SampleData::transients` (seconds).
- `Clip::grain`: Beats mode slices at the transients (0) or on a grid of 1/4 … 1/32. Sets from before M7 load with 1/16 (their old behaviour). Clip View → Sample: Slices.
- `Clip::warp` = false plays the file at its own speed from the first warp marker, whatever the tempo (recordings, one-shots). Clip View → Sample: Warp toggle (greys out the mode, BPM, slices and transpose).

### Background sample loading (`src/core/Sample.*`)

- `samples::loadMany(paths)` decodes what isn't in memory side by side (one thread per core) and fills the cache; `setfile::fromJson` uses it on every sample path in a set before reading the clips, so opening a set with many samples is ~cores× faster.
- `samples::loadAsync(paths, context, done)` decodes on the global thread pool and calls `done` on the UI thread (not at all if `context` was deleted). Drops on the Session View / Arrangement, the browser's double-click and preview, and drops on a Sampler use it; the clips go in when the audio is ready (by track id, in case tracks moved), with a busy cursor meanwhile. The cache itself stays UI-thread only.

### Tests

- Engine: Monitor Auto / In / Off and input channels, input meter; session and Arrangement audio recording with clicks at exact frames (beat placement within a few samples, rounded take length, WAV in the record folder, one-step undo); groups (bus level, solo both ways, group effects, ungroup + undo, fold, delete); clip envelopes (mixer and device targets); transient positions; Warp off timing at another tempo.
- Loading: `loadMany` (dedupes, reports failures), `loadAsync` (asynchronous, delivers on the UI thread, nothing to a deleted context).
- Window: Ctrl+click multi-select + Ctrl+G, no Arm on group strips, fold in the Session View → lanes close in the Arrangement → unfold there, Ctrl+Shift+G; Monitor / Audio From on an audio strip (undoable), strips stay level; envelope editor click / drag / undo / double-click / parameter chooser; Warp toggle + undo, Slices; Preferences input list. Round-trips: groups + fold, clip envelopes, Warp off, slices, Monitor, Audio From.
- `--screenshots` adds `11-group-and-audio-input`, `12-group-folded-arrangement`, `13-clip-envelope`, `14-warp-off`.

### Known gaps after M7

- Untested on real hardware in this pass: the PulseAudio capture stream, its latency figure and the compensation were checked against the test input only. Check a loopback (output → input cable or a monitor source) and adjust `AudioTake::comp` if takes land early or late.
- No input selection beyond the first two channels of one device; no per-track input device; no punch in / out, no comping or take lanes, no count-in.
- Group tracks don't nest; no group-level clip slot editing (the group's slots only launch); folding isn't undoable (by design).
- Clip envelopes are absolute only (no modulation envelopes) and can't be unlinked from the clip loop. Device panels and mixer controls don't show a clip envelope's value while it plays (they show Arrangement automation only).
- Transient detection has no sensitivity setting and no way to add / remove slices by hand.
- Background loading has no progress bar for very long files (a busy cursor only); opening a set still blocks until its samples are decoded (in parallel).

## Milestone 8: drums and performance (done)

Scoped from the M7 "next" list: the instrument-and-performance half. Real racks (parallel chains inside a device), multi-clip editing, Complex warp, recording polish beyond count-in / punch, themes and other platforms moved to Milestone 9.

### Drum Rack (`DeviceType::DrumRack`)

- `Device::pads` (16 `DrumPad`s: name, sample path, sample), pad k on MIDI note 36 + k. Parameters as data like every device: Volume, Velocity, then five per pad (`drumrack::param(pad, knob)`: Vol, Pan, Transpose, Decay, Choke), 82 in all, so every pad knob is automatable, MIDI-mappable and in clip envelopes. `kMaxParams` went from 32 to 96 and `deviceMergeKey` to 8 bits for the parameter (gestures moved to 200+).
- Built-in sounds (`src/audio/Builtin.*`): one-shots rendered once from the synthesized kits, named `builtin:<kit>/<note>` (kit / 808 / acoustic). A new rack is the 808 kit on all 16 pads. `samples::load / loadMany / loadAsync` understand the `builtin:` paths; the set file stores them as `{"builtin": "builtin:808/36"}` (no file, no relative path).
- Engine: `DeviceSlot::pads` holds the samples; `Context::pads` hands them to the processor. `DrumRack` (Instruments.cpp): 32 voices, one-shots (note-offs ignored), linear interpolation, Transpose = speed, Decay = -60 dB time (all the way up = no fade), choke groups fade the other pads of the group in ~4 ms. A voice whose pad sample was replaced stops (pointer check, as in the Sampler).
- Panel (`DrumRackBody`): `RackPads` (4×4, C1 bottom-left; click selects + auditions, drops load in the background onto that pad and the ones after it, right-click clears), `PadWave` (the selected pad's sample), one page of knobs per pad in a `QStackedWidget` (all exist, so the panel test finds every parameter). The selected pad is UI state per device (`devui::selectedDrumPad`, not saved).
- Loading: `import::toDrumPad / clearDrumPad`. `MainWindow::loadIntoSampler` sends a sample to the track's Drum Rack (its selected pad) before considering a Sampler, so the browser's double-click and drops on the Device View no longer replace a rack with a Sampler.
- The piano roll names rows after the track's Drum Rack pads. The browser lists the Drum Rack first in Drums.

### Macros (`DeviceType::Macros`)

- An audio-effect-kind device (so it goes on any track) with 8 knobs and `Device::maps` (`MacroMap`: macro, device id, parameter, lo, hi). One macro per target knob (mapping again moves it). Mapping the first knob to a macro sets the macro to that knob's value, so nothing jumps.
- Engine: `TrackData::links` (Macros slot, macro, target slot, parameter, lo, hi). In `runChain` a link wins over the target's own value, automation and clip envelopes; the macro's value itself follows its automation and clip envelopes.
- Mixer strips go back to the track's own settings once nothing drives them (a clip with a volume envelope stopped).
- `Project::drivenValue(track, target, beat, playing)` is the one place the UI asks "what sets this control now": a macro, else the playing clip's envelope, else automation (`Project::playingClip`, `Project::macroFor`). Device knobs (`DeviceBody::follow`, also on every `deviceChanged`), mixer strips and the Arrangement's header fields use it, so knobs now also show clip envelopes while a clip plays. Macro-driven knobs show their value in green.
- Panel (`MacrosBody`): the 8 knobs (named after their first mapping), right-click → Map to ▸ device ▸ knob, Map… for any macro, and a list of mappings with Min / Max (`DragValue`, merge keys 240+) and ✕. Duplicating a track remaps macros to the copy's devices.

### Count-in and punch in / out

- Engine: `Command::Play` with `a` = beats of count-in. While counting in, only the click plays (always, metronome or not), the playhead waits, and `playing()` already reports true; `countInLeft()` says how much is left. Blocks end exactly where the count-in does. The capture's start beat is stamped `m_beat - m_countIn`, so audio captured during the count-in maps to the beats before the start and is never in a take.
- Transport: `setCountInBars` (QSettings `record/countIn`; Preferences → Record and the metronome's right-click menu); a count-in happens when Play starts with Arrangement Record on, or when a slot's ● starts the transport. `countingIn()`; `positionText()` shows `-2. 1. 1` style while counting (red in the control bar). Nothing records during it.
- Punch: `setPunchIn / setPunchOut` (saved with the set: `transport.punchIn / punchOut`). Arrangement takes (Session clips, keys, audio) only exist inside the loop brace's range; a keys take starts exactly at the punch-in; everything is cut at the punch-out. Control bar ⇥ / ⇤ beside the loop switch; red marks on the brace and lanes.
- Fixed on the way: the transport switched the capture off whenever no audio take existed yet, so a take that began a tick later (after a count-in, at a punch-in) started late, and a take whose start was before the capture's first frame was never written. The capture now stays on while an Arrangement recording is about to take the input, and a take starting before the capture is padded with silence (`AudioTake::framed / stopKnown`).

### Tests

- Drum Rack: default pads, notes on C1 / D1 sound, no pad = silence, cleared pad = silence; on the processor: choke group on / off, Transpose +12 halves the length, short Decay, pad Volume -24 dB. Macros: Utility Mute via a macro (and back), an automated macro, `drivenValue` with ranges, duplicate remaps. Sets: pads (file + built-in), choke, maps and punch-in round-trip exactly; built-ins saved by name. Count-in: engine waits and clicks then plays; Arrangement audio recording with a count-in keeps the count-in out and the beat right. Punch: MIDI notes outside the range dropped, take from the punch-in; audio take exactly the punch range. Panels: pad click selects, drop onto a pad (one undo, undo restores), Macros list Min/Max edit (undoable), mapped knob follows the macro. Window: a browser sample lands on the selected pad, Punch-In switch, Preferences count-in.
- The device test now counts 29 devices; the panel tests find devices by name.
- `--screenshots` adds `15-drum-rack-and-macros`, `16-drum-rack-piano-roll`, `17-punch-in-out`; `devices/` has the Drum Rack and Macros.

### Known gaps after M8

- No real racks: Macros is a flat device (no chains inside a device, no parallel chains, no chain selector). The Drum Rack's pads are samples only (no instrument or effects per pad, no per-pad sends).
- Drum Rack pads have no start / end / loop or filter; Decay is a plain fade. No "Fold" to the pads in the piano roll. Pad selection isn't saved.
- Macros can't be renamed or turn mixer controls (only device knobs); a knob a macro turns can still be dragged by hand, but the macro wins.
- Count-in has no pre-roll of the song; punch has no take lanes (looping with punch records over the previous pass).
- Recording is still untested on real hardware (see M7).

## Milestone 9: racks (done)

Scoped from the M8 "next" list: racks with chains. Drum Rack pads holding instruments, multi-clip editing, MIDI CC envelopes, Complex warp, take lanes, themes and packaging moved to Milestone 10. This pass also wrote `docs/test-checklist.md`: everything since M5 that the tests can't hear or touch (real MIDI hardware, real audio input, how the devices sound).

### Model: racks in the flat device list (`Model.*`, `DeviceSpecs.*`)

- `DeviceType::InstrumentRack` (an instrument) and `AudioEffectRack` (an audio effect). Parameters as data: 8 macros, then 4 per chain for 8 chains (`racks::param(chain, Volume / Pan / On / Solo)`), 40 in all, so chain levels are automatable, mappable and in clip envelopes.
- A rack's chains are **not** nested containers: their devices follow the rack in `Track::devices` with `Device::rack` (the rack's id) and `Device::chain`, chain by chain, each chain in playing order; `Device::chains` holds the chain names. So everything that finds a device by id (automation targets, clip envelopes, MIDI Map, Macros, `drivenValue`, undo snapshots, the set file) works for devices in chains unchanged. `deviceBlockEnd` gives the end of a rack's block; `normalizeRacks` restores the order (sets from files; racks never nest, Macros never go inside).
- Operations, each one undo step: `addDeviceToChain` (an index past the last makes a chain named after the device; in an Instrument Rack chain MIDI effects go first, one instrument replaces another, audio effects go last; Audio Effect Racks take audio effects only), `addChain`, `deleteChain` (later chains' knobs move down with them), `renameChain`, `deleteDevice` (a rack takes its chains along), `groupIntoRack` (Instrument Rack for an instrument, Audio Effect Rack for an effect), `ungroupRack` (only with one chain or none). `addDeviceToSelected` treats a rack and its chains as one block (an Instrument Rack replaces the track's instrument and is replaced as a whole). `duplicateTrack` points the copy's chains at the copy's rack.
- Rack macros are `Device::maps` like the Macros device's (the engine's links and `macroFor` already handled any device with maps). Their targets are the rack's chain devices and its own chain knobs.
- Ready-made racks (`RackRecipe`, `rackRecipes()`): chains of named devices (presets allowed), chain levels, device knob settings and macro mappings. Each recipe is also a spec (a copy of the rack's) so its name has a type, help text and macro defaults; `makeDeviceBlock` builds the rack and its chains. Layered Keys, Lead Stack, Parallel Crush, Wide Space, listed in the browser's Sounds.
- Set file: `"chains"` on racks, `"rack"` / `"chain"` on devices in chains; `normalizeRacks` after the ids are fixed.

### Engine (`Engine.cpp`, `Devices.*`)

- `DeviceSlot` carries `rack`, `chain`, `rackType`, `chains`. `runChain` skips devices in chains; a rack runs them: an Instrument Rack copies the notes into a chain event list per chain (`m_chainEvA/B`, so one chain's MIDI effects never change another's notes), renders each chain into `m_chainL/R` and mixes it into the track; an Audio Effect Rack copies the input into each chain and mixes the chains (`m_rackInL/R`, `m_rackSumL/R`). No chains: silence / the dry sound. Off: an Instrument Rack goes silent (its instruments get `panic`), an Audio Effect Rack is bypassed. Chains that are off or not soloed still run, silently, so their notes end.
- `RackProcessor::mixChain`: Volume (fader law), Pan (balance), On, Solo, with gains that glide (~10 ms) and per-chain peaks (`Engine::takeChainPeak(rackId, chain)`).

### Device View (`DeviceView.*`, `ui/devices/RackBodies.cpp`)

- `RackPanel`: a frame holding the rack's own `DevicePanel` and the devices of the chain selected in its list (`devui::selectedChain`, per rack, not saved; `rackSignals()->chainSelected` makes the Device View rebuild), then a `ChainDrop` zone that adds a dropped device to that chain (or makes the first chain). `DeviceView::panels()` now finds panels inside racks too.
- `RackBody`: eight macro knobs (right-click → Map to ▸ chain ▸ device ▸ knob, or the chain's own Volume / Pan / On), a short mapping list with Min / Max, and the chain list: a row per chain with On, the name (click: show it; double-click: rename; right-click: Rename / Delete Chain), S, a Volume and a Pan field (`DeviceBody::field`, a compact `DragValue` bound to a parameter) and a meter. All 8 rows exist (those past the last chain hidden) so every knob has a control; dropping a device on a row adds it to that chain, elsewhere on the list makes a new chain.
- The macro mapping helpers moved from `MacrosBody` into `DeviceBody` (`macroLabel`, `macroMenu`, `fillMacroTargets`, `addMacroMap`, `macroMapRow`, …) so Macros and racks share them; a rack's own chain knobs are labelled like "Pad Vol".
- Device title bars: right-click → Group into Instrument / Audio Effect Rack, Ungroup Rack, Add Chain, Delete. The × uses `Project::deleteDevice`.

### Tests

- Sound: a two-chain Instrument Rack = the two synths added; a chain off / volume down / the other soloed = one synth; a Chord in one chain doesn't reach the other; hard-left pan; a macro on a chain's volume; Audio Effect Rack: empty chain = dry, two empty chains = twice, a muted chain adds nothing, bypass; Instrument Rack off = silence.
- Editing: Group (one undo step, same sound), no racks in racks, Audio Effect Rack refuses instruments, a new chain from a drop, order inside chains, deleting chains (knobs move), Ungroup rules, undo, deleting a rack, a rack replacing the instrument, recipes (chains, devices, maps), duplicating a track, the set file round trip, a broken order repaired.
- Device View: the rack and its first chain, switching chains, a drop next to the chain, one chain row per chain, a macro driving a pad's cutoff.
- `--screenshots` adds `18-instrument-rack`; `devices/` has both racks.

### Known gaps after M9

- No MIDI Effect Rack, no key / velocity zones or chain selector, no nested racks; Drum Rack pads still hold samples only.
- Devices can't be dragged between chains or reordered (delete and drop again); chains can't be reordered.
- Deleting a chain doesn't move the automation / clip envelopes / macro mappings of the later chains' knobs down with them (it does move their values).
- Macro and chain names can't be renamed apart from chains; a ready-made rack can't be saved from your own rack.

## Milestone 10: guitar, and playing through the app (done)

Asked for after the user's first hands-on test of 0.9 (30 of 54 checks tried: 27 pass, 2 fail, 1 skip): a Tuner, a guitar amp with heads, cabinets and an IR loader, metal-focused drive pedals and other pedals, each with its own panel; then "make sure I can select the inputs and outputs of my interface, and that I can have monitoring while playing through the app". Racks with instruments on Drum Rack pads, multi-clip editing and the rest of the old M10 list move to Milestone 11.

### Fixes from the 0.9 test

- **Pops when switching effects on / off** (3.14: Phaser, Auto Pan, Redux, Gate, Limiter): the engine crossfades every device's on / off over ~12 ms (`DeviceProcessor::onMix`, in `runDevice` and `runRack`): an effect fades between dry and processed, an instrument fades out and then gets `panic`.
- **Electric's tremolo lamps pulsed with nothing playing** (3.7): they glow only while voices sound (`meter[0]`).
- "Acid Line" (3.6) is a Sound in the browser; the Bass's help now says so.

### Guitar devices (`src/audio/Guitar.cpp`, `src/audio/Convolver.*`, `src/ui/devices/GuitarBodies.cpp`)

- Specs: `DeviceType::Tuner … Wah`, `isGuitarType`, `guitaramp::` indices; a 0–10 knob unit (`Unit::Dial`).
- **Oversampling:** every clipping stage runs at 4x (`Oversampler4`: 64-tap polyphase FIR up and down), so high gain doesn't fizz with aliases.
- **Tuner:** passes (or mutes) the sound; YIN on a 4x-decimated copy for the period, refined at the full rate with the normalised square difference (McLeod) on a low-passed copy and a parabola. Within ±0.5 cent on plucked test strings from 41 Hz (bass E1) to 330 Hz. `meter[0]` = Hz, `meter[1]` = level. Panel: note, cents, needle or strobe, a 5-second trace.
- **Guitar Amp:** five `HeadVoice`s (pre-gain range, tight low cut, bright, 1–3 asymmetric tube stages with interstage low-pass, tone stack voicing, power amp drive with sag, output trim, clean low blend for Bass), Bass / Mid / Treble / Presence as shelves and a peak, Volume drives the power amp. Cabinets: five biquad models plus the mic (on / off axis, far with two reflections), or a **loaded IR**: `makeIrKernel` (UI thread, cached per sample in `Engine::m_irCache`, energy-normalised, ≤ 8192 samples) and `Convolver` (uniformly partitioned overlap-save, 128-sample partitions, one block of latency, no allocation) via `Context::ir`. The IR is the device's `samplePath` / `sample` (saved like a Sampler's). `ampResponseDb` draws the tone stack + cabinet.
- **Pedals:** one `DrivePedal` for Overdrive (TS: clean + clipped mids), Distortion (RAT: huge gain, op-amp slew, hard knee, Filter), Metal Distortion (tight, two stages, 3-band EQ with sweepable mid), Chainsaw (HM-2-style low and upper-mid peaks), Metal Boost (tight boost), Fuzz (two starved stages, Muff tone), Crunch (biased tube, tilt, Bright); `Twang` (two dispersive spring loops per side + treble boost), `PhasePedal` (4 stages, Script), `ChorusPedal` (BBD-style, stereo taps), `Wah` (SVF band-pass, manual or envelope Auto, guitar / bass range). `pedalResponseDb` draws their filters. Levels at defaults are within ±3.5 dB of unity (tested).
- **Panels:** Tuner display; Guitar Amp with a head picker (five faceplates), the knobs on the head's colour, the tone + cabinet curve with in / out meters, a cabinet view (speakers, mic dot, or the IR's waveform), Cab / Mic choosers, Load IR (also drop a WAV anywhere on the amp); pedals as flat stompboxes, each with its face colour, lettering and decoration, a window (response curve, clipping curve, springs, rate light, the wah's rocker you drag), a light and a footswitch (switches the device, one undo step).
- **Rigs:** guitar `RackRecipe`s (Metal Rig, Swedish Chainsaw, Blues Crunch, Clean Twang, Bass Amp) with `guitar = true`; the browser's new **Guitar** category lists the Tuner, the amp, the pedals and the rigs (they stay in Audio Effects / Sounds too).
- **Create → Insert Guitar Track** (`Project::insertGuitarTrack`): an audio track on Input 1, Monitor In, with a Tuner and a Guitar Amp, one undo step.

### Audio interface I/O and monitoring (`src/audio/JackDriver.cpp`, `Driver.h`, `Engine.*`, `Preferences.*`)

- **JackDriver:** the JACK API loaded at run time (PipeWire's `pipewire-0.3/jack/libjack.so.0` first, then a JACK install's), duplex: one callback gets Input 1 / 2 and fills Main L / R for the same period. The buffer is asked for with `PIPEWIRE_LATENCY` and `jack_set_buffer_size`. `Engine::process(out, frames, in)` takes the input straight into `m_inBlock` (no ring, no cushion), so monitoring through a track costs two periods plus the interface: measured ~10.7 ms at 128 samples on the user's machine (4.0 ms in, 6.7 ms out).
- Ports: the user picks Main out L / R and Input 1 / 2 among the physical ports (`listJackPorts`), or Automatic: the first analog output, and inputs **of the same device as the output** (the user's onboard input gave full-scale junk; their UMC202HD is the output). Outputs are connected first; each input is then checked: if the callbacks stop (PipeWire stalls the graph on an input with nothing behind it), it's disconnected again and `Driver::problem()` says why and how to fix the interface's profile. Preferences → Audio shows it.
- **The user's machine:** PipeWire 1.0.5 at 48 kHz, Behringer UMC202HD on the profile "Analog Stereo Output + Digital Stereo (IEC958) Input". Its "IEC958" input does deliver the analog inputs in our tests, but **Analog Stereo Duplex** (profile 1) or **Pro Audio** (12) is the right profile; the checklist tells them how (`wpctl set-profile <device> <profile>`). Sessions never changes system audio settings itself.
- Settings: `audio/system` ("jack" default when a libjack exists, else "pulse"), `audio/frames`, `audio/out1/2`, `audio/in1/2` (`""` automatic, `"-"` none); `PreferencesDialog::savedAudio()` composes the `DriverOptions`. If JACK can't open (no library, no server, another rate) the engine falls back to PulseAudio and says why.

### Tests

- Fades (an effect switched on doesn't jump), instrument off silent after the fade; every guitar device's level at defaults; Tuner accuracy (E1, E2, A2, E4, +12 cents), silence, Mute; IR convolution against direct convolution at odd block sizes; amp: gain adds harmonics, the cabinet rolls off the fizz, the drawn response, Loaded IR = amp output convolved; Wah heel / toe; Chainsaw's peak; the guitar track (devices, input, monitoring, one undo, input heard through a duplex `process`); the footswitch; an IR dropped on the amp; saved audio settings. The JACK driver itself was checked by hand on the user's PipeWire (ports listed, 128-sample duplex, latencies, the stalled-input guard).
- `--screenshots` adds `19-guitar-track`; `devices/` has all 44 devices.

### Known gaps after M10

- Two inputs and two outputs (Main) only; no per-track output routing, no cue / headphone mix, no direct monitoring toggle per interface.
- No sample-rate change in Sessions (it runs at 48 kHz; JACK at another rate falls back to PulseAudio).
- The Tuner has no polyphonic mode; the amp has no noise gate of its own (use Gate) and no power-amp tube choice; IRs are trimmed to 170 ms.
- Pedals are fixed-voice models; no pedal order drag (delete and drop again), no stereo amp modelling beyond processing L and R alike.

## Milestone 11: dragging devices around, after FORM (done)

Asked for: "take a look at FORM, see what it does well and implement it … Things that I miss for instance is drag and drop of instruments and such". FORM (`~/Documents/creative-projects/daw`, the user's other DAW, JUCE / C++20, **read only**: never write there) was reviewed from its README, feature list and `src/Studio.cpp`. What it does well that Sessions lacked, and what came over:

| FORM | Sessions 0.11 |
| --- | --- |
| Library items dropped on a track (Session column, Arrangement row, mixer) or at a position in the chain, with a marker | the same, with the kind order kept (`devicedrag::drop`, the orange line in the Device View) |
| Dropping into the empty area creates a track | a MIDI track for instruments / MIDI effects / Sounds, an audio track for effects, named after it, one undo step |
| Device headers drag to reorder; dragging onto another track transfers | title bars drag: along the chain, into / out of a rack's chain, to another track (automation goes along), Ctrl copies |
| Ctrl / Shift-click headers to select several; Ctrl+G rack, Ctrl+Shift+G ungroup | the same, plus Delete, Ctrl+D, Ctrl+C / Ctrl+V on the selection |
| Double-click a header to fold | the same (`Device::folded`, saved, one undo step) |
| `.formpreset` device / rack presets, Presets browser, file drop | `.sessionspreset` (one device, a rack, or several in a row), browser **User Library**, drop from the file manager, rename / delete |
| Chain auto-scroll while dragging near its edges | the same |
| Scene labels drag to move rows | the same (a click on the name still launches) |

Seen in FORM and **not** brought over (candidates for later): take lanes and comping; native sidechain inputs (Compressor / Gate / Auto Filter keyed from another track); Dark / Mid / Light themes and custom colours; dragging a Session clip into the Arrangement (Tab while holding); the Orbit synth's drag-a-source-onto-a-knob modulation; typing a value after Ctrl+click on a knob; external VST3 / LV2 / LADSPA plugins; a Monitoring tab with input / post-FX meters; crash recovery and automatic backups.

### Model (`Model.*`)

- `Project::DevicePlace {beforeId, rackId, chain}`: where devices go. Top level: before the block of `beforeId` (0 the end, -1 their natural place, the end of their kind's section); with `rackId`: into that chain (-1 / past the last: a new chain named after the first device), before `beforeId`.
- `placeBlocks` (Model.cpp, private) is the one placement routine: it clamps each block into its kind's range (MIDI effects < instrument < audio effects, `rankRange`), replaces an existing instrument (one per track or chain), refuses kinds the track can't take (`why` says so), racks inside racks, Macros in a chain, a full rack. Callers work on a copy of the device list and commit only when everything fits.
- `insertDevices(to, blocks, at, label)`, `moveDevices(from, ids, to, at, copy)`, `deviceBlocks(track, ids)` (a rack with its chains; a chain device inside a selected rack stays with it), `freshCopies` (new ids, racks' members and macro maps follow), `canPlace` (drag feedback), `insertTrackFor(blocks, name)` (after the last track, outside groups), `deleteDevices`, `setDeviceFolded`, `groupIntoRack(track, ids)` (several into one chain where the first was; an instrument makes it an Instrument Rack), `moveScene`.
- `pruneMacroMaps`: after any move, a rack keeps only mappings to itself and its chains' devices, a Macros device only those to devices on its track. Moving to another track takes `device:<id>:*` automation along (ids stay; the engine keys DSP state on them, so a moved device keeps its state). Clip envelopes stay with their clips. MIDI / key mappings name the track id; they're not rewritten.
- `addDeviceToSelected` and `addDeviceToChain` now go through `insertDevices` (natural place / chain end). MIDI effects added by double-click now go after the other MIDI effects rather than first.

### Drag and drop (`ui/DeviceDrag.*`, `DeviceView.*`, `SessionView.cpp`, `ArrangementView.cpp`, `RackBodies.cpp`)

- `devicedrag::Payload`: browser items (`Browser::mimeType()`, kinds i / f / m), presets (`path|u`, or `.sessionspreset` URLs) and device panels (`kMime` = `pid|trackId|id,…`; another process's pid is ignored). `accepts` checks with `canPlace` (a preset is read once per file version, cached), `drop` does it in one undo step (a new track and its devices too) and returns what to show in the Info View (`InfoView::notify`, the window's Info View from anywhere).
- Device View: `placeAt(pos)` finds the place and the line (left half of a panel: before it; next to a rack's own panel: into its shown chain); the line is a 3 px accent widget on the host. `dragDevice` runs the QDrag from a title (`DeviceTitle`); while it runs, `rebuild` is deferred (`m_panelDrag`), so the title isn't deleted under its own mouse handler; the pending rebuild runs from a zero timer after. Selection: ids of the shown track, chain-ordered (`selection()`), Shift ranges within one level. Titles paint their own background (selected: lighter). Folded panels are 24 px with the name on end and no body (`body()` is null: check it).
- Right-click menus run their command after `QMenu::exec` returns, with a parentless menu (the panel may be gone).
- Session: `ClipGrid::deviceDragOver / deviceDrop` (column outline; the free space = new track); the mixer's host takes drops through `SessionView::eventFilter`. Arrangement: `deviceLaneAt` (lanes and headers; the Main lane and below = new track).
- `MainWindow::Area::Devices`: Delete, Ctrl+D, Ctrl+C / V, Ctrl+G / Ctrl+Shift+G act on the device selection when the Device View has focus (a title click focuses it).

### Presets (`core/Presets.*`, Browser)

- JSON `{"format": "sessions-preset", "version": 1, "name", "devices"}`, devices as in a set (`setfile::deviceToJson / deviceFromJson`, now public). Read: normalizes racks, drops macro maps to devices it doesn't hold, restarts broken ids. Folder: `QStandardPaths::AppDataLocation/Presets` (`~/.local/share/Sessions/Sessions/Presets`), `SESSIONS_PRESETS` overrides it (the tests and `--screenshots` use a temporary one). `presets::Library::changed` rebuilds the browser.
- Samples in presets are absolute paths (no companion folder yet).

### Tests

- Model: before / after / end / own place (no undo step), kind order, MIDI effect at the end, automation moving and undo, copy (new id, same knobs), refusals with a reason, instrument replacement, into / out of a rack chain (macro maps pruned), rack to another track, several at once, grouping two (sound unchanged), fold saved, deleting several, scenes and undo.
- Presets: save / list / name, unsafe names, read back, dropped with fresh ids and the rack's macros following, found by the browser's search, rename, file URLs.
- Device View: the line's position, a drop there, title click / Ctrl / Shift selection, a two-device drag to the end, Ctrl copies, Delete / Duplicate / Copy-Paste to another track, fold / unfold by double-click, an instrument refused on an audio track, a drop next to a rack into its chain. Session: column, refusal, free space (MIDI track named after the Sound; audio track for an effect; one undo), a panel's drag moving / Ctrl copying across tracks, a mixer strip, scene drag and click. Arrangement: header, Main lane. Window: focus routing of Ctrl+D, Delete, Ctrl+G.
- `--screenshots` adds `20-devices-selected-user-library`.

### Known gaps after M11

- No drag of a device onto a specific place in another track's chain without selecting that track (it goes where its kind goes there). Hot-swap (replacing a device in place from the browser) isn't there.
- Presets keep samples by path; moving the samples breaks them (they load silent, like a set with missing files).
- Dragging from the Device View onto another track uses Qt's drag, so the Device View doesn't follow the pointer to the other track; the result shows once dropped.
- No MIDI Effect Rack; no drag of whole chains between racks.

## Milestone 12: take lanes and sidechains, after FORM (done)

Asked for: the two FORM strengths still missing after M11, "take lanes and comping" and "sidechain inputs for Compressor, Gate and Auto Filter", with the engine processing tracks in dependency order and never allocating on the audio thread. Read in FORM (read only): `docs/sidechains-v0.12.md`, `src/Routing.cpp` (`compileSidechains`: the DFS order, cycles refused), `src/SidechainDSP.h` (detector gain / HP / LP, the 5 ms Listen blend) and `src/Model.cpp` (`selectComp`, the comp read with its crossfade). Also asked for, after this: a Massive-style synth (see "Milestone 13: next").

### Take lanes and comping (`Model.*`, `Transport.*`, `Engine.cpp`, `ui/TakesEditor.*`, `ClipView.*`)

- **Model:** `Clip::takes` (`Take`: samplePath, sample, name, `startBeat` = the clip beat where that pass's file begins) and `Clip::comp` (`CompRegion` start / end / take in clip beats, sorted, never overlapping). `takeAt(beat)`: the region's take, else the newest; `useTake(start, end, take)` cuts what's there around the new region and joins neighbours of the same take; `useWholeTake`; `takeSeconds(take, beat)` = `secondsAt(beat) − secondsAt(startBeat)`. With takes, `samplePath` / `sample` mirror the newest take, so everything that treats a clip as "a file" (the Sample box, Missing Samples, Collect All) keeps working, while the engine plays from the takes. Equality, undo snapshots and `copyClip` carry both.
- **Recording** (`Transport`): with Arrangement Record and the loop on, the first pass that runs to the loop end makes one clip over the loop (`m_loopClips`, per track id for this run; cleared at Play, Record on / off and `reset`); every later pass goes in with `addTake`: a full pass clears the comp (the newest plays everywhere); a pass cut short by Stop first makes the old newest explicit, then takes over where it has audio. A pass begun mid-loop has `startBeat > 0` (silence before it). Every pass is under the same `recordMergeKey`: one undo removes the run; the files stay. The loop wrap now ends takes exactly at the loop end (`finishTakes(endAt)`) and the next pass begins at the loop start (`m_recStart = m_loopStart`).
- **Fixed on the way:** a take begun after a loop wrap mapped its beats onto the capture's frames as if beats never wrapped (`startFrame = (startBeat − cap0) · fpb`), so a second pass over a loop got the first pass's frames (or silence) instead of its own audio. `AudioTake::linear` (what `m_wrapBeats` was when the start was set) makes beats count on through wraps.
- **Monitoring replaces clips** (FORM does this): with Monitor In, or Auto while armed, an audio track plays its input *instead of* its clips, so the pass you're playing isn't heard over the one before. Disarm (or Monitor Off) to hear what you recorded. The engine skips `renderAudio` for such tracks.
- **Engine:** `ClipData::takeSamples / takeStart` and the comp as segments (`segEnd / segTake`: the regions, gaps filled with the newest take, same-take neighbours merged). `renderComp()` reads the segment's take at `secondsAt(local) − secondsAt(startBeat)` (Warp off: beats × seconds per beat) and crossfades ±2.5 ms (`kCompFade` 5 ms, linear) with the neighbouring segment's take; the existing edge fades still handle the loop seam and placement ends. Beats-mode slicing and Reverse don't apply to takes (Reverse is disabled for them in the Clip View).
- **Takes view** (`TakesEditor`, index 3 of the Clip View's editor stack; Sample box → **Takes (N)**): a ruler, the **Comp** lane (each stretch drawn from the take it plays, in that take's shade: `takeColor`), then one lane per take, newest first, bright where the comp uses it. Drag across a lane = `useTake` (snapped to 1/4 beat above 60 px/beat, else a beat; Alt: free), click = one grid step, double-click or right-click → Use Whole Take, right-click → Newest Take Everywhere, wheel scrolls the lanes. Each edit is one undo step. The sample editor and the Arrangement draw take clips through the comp.
- **Set file:** clips get `"takes": [{path, relativePath, name, start}]` and `"comp": [[start, end, take], …]` (no version bump: older readers play the newest take's file). `samplePaths()` scans `"takes"`, so they're decoded up front with the rest and Missing Samples lists them.

### Sidechains (`DeviceSpecs.*`, `Model.*`, `Engine.*`, `Effects.cpp`, `ui/devices/EffectBodies.cpp`)

- **Specs:** Compressor, Gate and Auto Filter each get five appended parameters (`sidechain::Tap / Gain / HP / LP / Listen`, `sidechain::param(type, knob)`, base 8 / 6 / 8): Key Tap (Pre FX / Post FX / Post Mix), Key Gain ±24 dB, Key HP (20 Hz – 2 kHz; all the way down = off), Key LP (1 – 20 kHz; all the way up = off) and Listen. As parameters they're automatable, mappable, in clip envelopes and presets.
- **Model:** `Device::sidechainTrack` (0 = the device's own input). `canKeyFrom`: a regular, non-group track other than its own, and no loop (`keyReaches` walks the keys); `setKeySource` is one undo step; `moveDevices` refuses a keyed device onto its key's track or into a loop. A deleted source keeps the id ("Missing", a silent key; undo brings it back). Presets drop the track (`presets::read`). Set file: `"sidechain": <track id>`.
- **Engine:** `publish()` resolves every keyed slot to a track index (`DeviceSlot::keyTrack`), marks the sources (`TrackData::keySource`) and orders the regular tracks by DFS so sources render first (`RenderState::order`; a loop, which the model refuses, is cut: that track reads the previous block's key). While a track is a source, its **Pre FX** (before the first top-level audio effect: clips + input, or the instrument's sound), **Post FX** and **Post Mix** (after fader and pan, so mute and solo apply) audio is copied into the engine's key store (`keyL(tap, track)`: ~200 KB on the heap, sized once in the constructor; the Engine object stays small enough for the smoke test's stack under ASan). `runDevice` hands the chosen tap to `Context::keyL / keyR`. Returns and group tracks can be keyed (they render after every regular track) but can't be sources. `renderOffline` sets `m_exporting`, which zeroes Listen in the snapshot.
- **Processors:** `KeyDetector` (Effects.cpp): the key is `ctx.keyL/R` or the device's own input, × Key Gain (5 ms smoothed), through one-pole HP / LP; the detector reads `level(i)`; `audition()` swaps the key into the output under Listen with a 5 ms blend. `meter[2]` = the key's peak in dB (`DeviceProcessor::meter` has three entries now; `Engine::deviceMeter` takes 0–2). The Compressor's curve dot (`meter[1]`) and the Gate's history now show the key.
- **Panels:** `sidechainSection()`: a **Key From** chooser (`KeySource`: Internal and the tracks; ones that would loop are greyed out; a missing source shows in red), the Key Tap chooser, Gain / HP / LP knobs, Listen, and a **Key** meter (`KeyMeter`). Compressor 510 px wide, Gate 470, Auto Filter 574.

### Tests (65 new; 506 in all)

- Comp: the newest by default, cutting / joining regions, Use Whole Take, take time through the warp map. Engine: a comped clip's stretches, the 5 ms crossfade (no step at the join), a missing take file. Monitoring replacing clips (In, Auto + armed, disarmed). Loop recording: one clip over the loop with three full passes and one cut short, each pass's audio on its beat, the short pass playing where it has audio, playback of the comp, comping an older pass back in, one undo, the files kept, the set round trip (`captureEditState ==`, names, audio). Takes view: lanes, a drag (snapped, one step), a click (one grid step), undo, double-click; the Clip View's Takes button, Show taking over, a clip without takes.
- Sidechains: the appended parameters; who can key whom (own track, EQ, returns, groups, loops, chains of keys), one undo step, moves refused; the engine: a muted kick ducking a bass through Post FX, the key arriving in the kick's own block though the kick is later in the track list, Post Mix following mute, Pre FX vs Post FX around a shut Gate, Key Gain and Key HP, Listen, export ignoring Listen, the key meter, a keyed Gate and Auto Filter, a keyed return, a deleted source and undo; the set file and a preset; the Compressor panel (Key From, mappable Listen, the meter, fits).
- `--screenshots` adds `21-takes-comping` and `22-sidechain-compressor`; `devices/` shows the three panels with their Sidechain column.

### Known gaps after M12

- Takes come from Arrangement loop recording only; recording into a Session slot keeps one take. No renaming or deleting takes, no comp for MIDI clips, no flattening a comp to one file, no per-join fade length (fixed 5 ms).
- Take clips always read like Re-Pitch (no Beats slicing of takes); Reverse is off for them.
- A key is the source's current block: no lookahead, no latency compensation between tracks. Groups and returns can't be sources; a Post Mix key from a track inside a group is that track's own output, not the group bus. Key HP / LP are first-order. Listen goes through the target track's fader (like FORM), so it's silent on a muted track.
- Key HP / LP knobs print "20.0 Hz" / "20.0 kHz" at their off positions rather than "Off".

## Milestone 13: Monolith, the big synth (done)

Asked for right after M12: "an incredibly powerful synth, capable of creating meaty growling sub basses, much in the spirit of Native Instruments Massive, with automations, several oscillators, steps and so much more". Built as one Sessions instrument, `Monolith`, with everything automatable the Sessions way (every knob is a parameter: Arrangement automation, clip envelopes, MIDI Map, Macros / rack macros), plus its own modulation routes.

### Parameters as data (`DeviceSpecs.h` `monolith::`, 112 knobs; `kMaxParams` is now 128)

- **Voice** (0–7): Volume, Glide, Voice (Poly / Mono / Legato), Unison (1–4), Detune, Spread, Vel > Amp, Key > Cutoff.
- **Osc 1–3** (`oparam(o, k)`, 8 each): Table (the eight wavetables), Pos, Mode (Bend / Formant / Sync), Intensity, Pitch (±36 st), Fine (±100 ct), Level, Filter (0 = into filter 1, 1 = into filter 2).
- **Mod Osc**: Pitch (±36 st from the note), Phase (phase-modulates all three oscillators), Ring. **Noise**: Level, Color. **Feedback** (the previous sample back into filter 1).
- **Filters 1–2** (`fparam`): Type (LP 12, LP 24, HP, BP, Notch, Comb, Scream), Cutoff, Reso; **Routing** (0 parallel … 1 filter 1 into filter 2), **F Mix** (0 filter 1 … 1 filter 2); **Insert** (Off, Tanh, Soft, Hard, Fold, Bits) + Amount.
- **Env 1–4** (`eparam`: A, D, S, R; Env 1 is the amp). **LFO 1–2** (`lparam`: Rate = the sync divisions or Free, Hz, Shape). **Step 1–2** (`sparam` / `step(s, i)`: Rate, Steps 1–16, Glide, 16 step values −100..100 %). **Macro 1–4**.
- `Device::mods` (`ModRoute` source / param / amount): the routes. Sources (`monolith::Source`): Env 1–4, LFO 1–2, Step 1–2, Macro 1–4, Velocity, Key, Random (LFOs, steppers, Key and Random swing −1..1; the rest 0..1). An amount is a share of the knob's range, bipolar; a modulated knob's value shows in cyan. Up to 32 routes. Saved as `"mods"` on the device; presets keep them; `freshCopies` / `duplicateTrack` copy them; `Preset` (the Sounds table) gained `mods`, applied by `makeDevice` through `presetMods(name)`.
- **Sounds:** Growl Bass, Wobble Sub, Reese Monolith, Neuro Step, Formant Talk, 808 Sub, Sync Bass (`monolithPresets()` in DeviceSpecs.cpp), each with routes.

### DSP (`src/audio/Monolith.cpp`, `Wavetables.cpp`)

- Eight voices (one in Mono / Legato, with a held-note stack and glide like the Bass device). Per voice: 3 oscillators × up to 4 unison phases (detuned, spread across the stereo field, 1/√U normalised), each read from its wavetable at the band limit of its *warped* speed, through `monolithWarpPhase` (Bend: the first half of the cycle squeezed, the second stretched; Formant: the cycle squeezed to the left, the rest held; Sync: the cycle restarted up to 8× per note), phase-modulated by the sine mod osc and ring-modulated by it; noise through a one-pole (Color); the previous output sample × Feedback back into filter 1. Each oscillator's Filter knob splits it between the two filters; Routing feeds filter 1's output into filter 2; F Mix balances them. Filters (`VoiceFilter`): SVF LP 12, two-stage LP 24, HP, BP, Notch, a Comb with its own 4096-sample line (Reso = feedback), Scream (tanh drive scaled by Reso into a resonant LP 24). Then the insert (`dsp::saturate`), Env 1 × velocity × 0.3, and a smoothed Volume.
- Modulation: every `kSub` = 8 samples each voice copies the block's knobs, adds its routes (`source()` reads the voice's envelopes / velocity / key / random, and the shared LFO, stepper and macro values) and re-derives everything from the result (`modulate()`). LFOs are synced to `Context::beat` (which free-runs while stopped) or run free at the Hz knob; steppers pick `floor(beat / rate) mod Steps`, with Glide a one-pole over half a step. `meter[0]` voices, `meter[1..2]` the LFOs.
- Four new wavetables for basses (`Wavetables.cpp` cases 4–7): **Sub** (a sine growing a few warm harmonics), **Growl** (a saw with a resonant bump sweeping up through the harmonics), **Metallic** (sparse, clangorous partials), **Digital** (stepped, bitty). The Wavetable Synth keeps its four (its Table choice is unchanged, so old sets keep their sound).
- Cost: ~18 ms per second of audio at the defaults (a four-note chord), under the Guitar Amp. The processor keeps its `DeviceSpec` at hand (`rv` / `rc`): `paramValue(type, …)` searches every device for the spec, which is fine per block but not forty times per voice per eight samples (`paramValue(const ParamSpec&, v)` is the cheap form).

### Panel (`InstrumentBodies.cpp`, `MonolithBody`)

Six pages in the 192 px device height, chosen on the left (remembered per device, not saved): **Osc** (three columns: table chooser, the warped cycle (`MonoWaveDisplay`, drag = Pos), Bend / Formant / Sync, Pos / Intensity / Level / F1-F2, Pitch / Fine; then Mod Osc, Noise and Feedback), **Filter** (two responses with a draggable dot (`MonoFilterDisplay`), Routing / F Mix / Key, the Insert), **Env** (four `AdsrDisplay`s), **LFO** (two LFOs with their shape and value, two steppers as 16 draggable bars (`StepDisplay`, the playing step lit)), **Mod** (source chips to drag, Add… menu, the routes list with an amount field and ✕ per route, the four macros), **Voice**. Every step value also has a (hidden) knob, so the "every parameter has a control" rule and MIDI mapping hold. Drop a `SourceChip` (`kModSourceMime`) on any knob: `eventFilter` adds a route at 30 % and shows the Mod page; right-click a knob: Modulate by ▸, and Remove for its routes. `addRoute / removeRoute / setRouteAmount` are each one undo step (`editMods`).

### Tests (36 new; 542 in all)

- Knobs in order; defaults audible and bounded; silence with no oscillators; every table sounds; the three warps add harmonics (second-difference curvature); LP 12 / LP 24 / HP / Scream / Comb; Env 2, a synced square LFO, a 4-step sequencer and Macro 1 as routes; Legato (no retrigger, glide C2 → G2 by zero crossings), Mono (retrigger), Poly (two voices); Unison width; feedback stability; ring mod, noise and Color, Bits, Vel > Amp; every Sound plays through the engine with its routes; routes through the engine (a macro silencing the oscillators); set file, preset and copy round trips; the panel: six pages, the Mod page's rows, a dropped chip, the amount field, ✕, undo, a modulated knob.
- `--screenshots` adds `23-monolith-synth` (the Mod page of Growl Bass) and `devices/12-monolith[-page].png` for all six pages.

### Known gaps after M13

- No per-route source scaling (a second source multiplying a route's amount, Massive's "side-chain" modulation), no modulation of a choice (Table, Type, Shape) that reads sensibly, no pitch-bend or mod-wheel input (Sessions has no MIDI controller streams beyond CC mapping).
- Steppers and LFOs are global, not per voice; LFO phase can't be restarted per note; there's no sample-and-hold "performer" with per-step curves.
- The Comb filter is a plain positive-feedback comb; no Scream-style saturation inside the ladder, no filter FM. Unison is capped at 4.
- The routes list doesn't show the live modulated value, and knobs don't draw their modulation range.

## Milestone 14: themes, and Session clips into the Arrangement, after FORM (done)

Asked for: the two FORM strengths still missing after M13, a light theme (and FORM's Mid) and dragging a Session clip into the Arrangement. Read in FORM (read only): `src/StudioAppearance.cpp` (`applyScheme`: the eight roles of the Dark / Mid / Light schemes, `visual::field()` / `readout()` deriving the rest; its UI checks: light surfaces, readable readouts, white and black piano keys, dark waveform ink) and `src/Studio.cpp` (`dragRole == 13`: the Session clip drag that keeps going after Tab and lands in the timeline as a copy at the snapped beat).

### Themes (`core/Theme.h/.cpp`, `ui/devices/DeviceBody.h`, every view)

- **One table, three columns.** `SESSIONS_THEME_COLOURS(X)` in `Theme.h` lists every colour with its Dark, Mid and Light value (`#rrggbb` or `#aarrggbb`); each becomes a mutable global (`theme::bg`, `theme::slotEmpty`, `theme::devDisplay`, …) initialised to the Dark value. `theme::apply(kind)` reassigns them all, re-applies the stylesheet (built from the same table through `{name}` placeholders), emits `theme::notifier()->changed()` and repaints every widget, so the theme switches live, no restart. `theme::kind()`, `savedKind()` / `save()` (QSettings `ui/theme`: dark / mid / light; `main()` applies the saved one before the window exists). **View → Theme** is a radio group of the three.
- **The dark theme is pixel-identical.** The Dark column holds exactly the literals the views used to carry; the ~250 hex literals in the views (Session grid, Arrangement, piano roll, sample / takes / envelope editors, mixer strips, faders, meters, knobs, the Device View's hand-made stylesheets, device bodies) became named entries, grouped by role (`editorBg`, `ruler`, `rulerText`, `barLine`, `lane*`, `roll*`, `dev*`, …). Where two concepts shared a value they got two entries rather than one, so Dark never moved. Checked by capturing `--screenshots` before and after and diffing: every static region matches the 0.13 binary; what differs is animation (meters, clocks, LFO displays, the stopped playhead's phase against the silent fallback clock).
- **Light and Mid** take FORM's eight base roles (bg / surface / raised / line / text / muted / accent / mint); accents are a little deeper than FORM's for contrast (`#e0901e`, `#f0b43c`), the device palette's cyan is FORM's mint. Everything else is derived by hand: light editors and device displays, dark grids and text, dark `outline` / `playhead` (every `Qt::white` selection outline, playhead and focus ring now reads those), `readout` = the text colour (value fields and `DragValue` numbers aren't orange on light surfaces, as in FORM), meter troughs stay dark, the track palette, clip ink, transport colours, envelope red and the guitar pedals' hardware colours don't change.
- **Device panels:** `devui::col::*` are references to the `theme::dev*` entries; the Device View rebuilds its panels on `changed()` (their stylesheets are built from the theme on the way in). `ControlBar` and the Arrangement's Back-to-Arrangement button rebuild theirs the same way; the browser's section titles take `theme::neutral`.

### Session clips into the Arrangement (`ui/SessionView.*`, `ui/ArrangementView.*`, `MainWindow::showSession`)

- Pick up a clip in the Session View as for a slot move, press **Tab** while holding it: `MainWindow::showSession(false)` sees `ClipGrid::clipDragActive()`, takes the clip from the grid (`takeClipDrag`, which ends the grid's drag) and hands it to `ArrangementView::beginClipDrag`, which `grabMouse()`s (the press happened in another widget) and follows the pointer. The view draws the clip itself where it would land (`drawClip` at 60 %, in the target track's colour, with the snapped start as an accent line and a dashed outline) or says "Drop it on a MIDI / audio track". The release calls `placeDraggedClip`: `clipDropLane` (a track of the clip's kind, not a group, not a return, inside the lanes), `snap(xToBeat(x), Alt)`, `arrangement::clearRange` under the new clip (like Paste), `copyClip`, select it, `clipSelected` → the Clip View shows it. One undo step, "Drag Clip to Arrangement"; the slot keeps its clip.
- **Tab again** (`showSession(true)` with `clipDragActive()` in the Arrangement) returns to the Session View with the clip still in hand (`ClipGrid::resumeClipDrag` grabs the mouse and carries on: drop it on a slot to move it as ever). **Escape** drops a clip in flight (the event filter). Any way of switching views goes through `showSession`, so the menu and the A key behave the same.
- Sessions shows one of the two views at a time, so Tab is the route (as in FORM); there's no second window to drag across.

### Tests (36 new; 578 in all)

- Themes: Dark is the default with the original values and the device palette aliasing the theme; View → Theme → Light switches everything (light surfaces, dark text, light displays, dark outlines and readouts), is remembered in QSettings, rebuilds the stylesheet, and the window really paints light (mean brightness of the grabbed Session View); the piano roll keeps white and black keys on a light grid; Mid sits between; back to Dark the grab is pixel-identical to before; the Dark stylesheet reads as the original literals. The block applies the stylesheet first: the earlier tests run unstyled, as they always did. `SESSIONS_SMOKE_DUMP=<dir>` saves its three grabs (before / Light / after) to look at.
- The drag: picking up, Tab hands over, the ghost paints, where it may land (not returns, ruler, headers), the snapped drop as a copy with its own id, one undo step, selected and shown, the slot kept, undo / redo, Alt off the grid, a drop onto an existing clip trims it (one undo restores both), a MIDI clip over an audio lane (forbidden cursor, nothing placed, no undo step), Tab back and a slot move, Escape.
- `--screenshots` adds `24-light-theme-session`, `25-light-theme-arrangement` (with the Device View), `26-mid-theme-session`, `27-session-clip-into-arrangement` (the ghost over the Pad lane) and `28-session-clip-dropped`.

### Known gaps after M14

- No custom colours (FORM's "Customize colors" picker), no per-role editing; three fixed themes.
- The Light / Mid values are hand-picked once; some panels could use another pass (the Guitar Amp's and pedals' faceplates keep their dark hardware look on a light panel on purpose).
- The dragged clip's ghost doesn't auto-scroll the Arrangement when the pointer nears an edge, and a drag can't start in the Arrangement and end in a Session slot.
- Dropping takes one clip at a time; a multi-clip (a whole scene) drag into the Arrangement isn't there.

## Milestone 15: NAM, Neural Amp Modeler captures (done)

Asked for: "can you find a way to implement NAM", the open neural amp-capture format (Steven Atkinson's Neural Amp Modeler; ToneHunt holds thousands of `.nam` captures of real amps and pedals), with the inference in plain C++ inside Sessions and no third-party library. The weight order and the layer math were taken from NeuralAmpModelerCore's sources (FORM carries a copy under `daw/vendor/NeuralAmpModelerCore`, read only) and checked against its example models: a scratch build of the core (with its vendored Eigen) was the oracle, and Sessions' engine matches it sample for sample (relative error about 1e-6, float rounding plus the fast tanh) on the classic small WaveNet, the 16 / 8 standard model, the full-width slimmable one, the LSTM, the A2 container (both sizes), the mixed LSTM / WaveNet container, and the user's own ToneHunt capture (`~/Desktop/Jcm800.nam`, an A2 container: full 9e-7, light 1.4e-6).

### The reader and the networks (`src/audio/Nam.h/.cpp`, namespace `sessions::nam`)

- **Format.** A `.nam` is JSON: `version`, `architecture`, `config`, a flat `weights` array, optional `sample_rate` (48 kHz assumed when missing, as the plugins do) and `metadata` (`name`, `loudness` in dB, `gain`, `input_level_dbu` / `output_level_dbu`, `gear_make` / `gear_model` / `gear_type` / `tone_type`, `modeled_by`). `nam::parse` reads it with `QJsonDocument` into an immutable `Model` (shared; `nam::load` caches by path, modification time and size); the file's weights must be used up exactly, else the error says how many the shape needs.
- **Architectures.** WaveNet (layer arrays with `input_size`, `condition_size` 1, `channels`, `bottleneck`, `kernel_size` or a `kernel_sizes` list, `dilations`, `activation` as a name, an object or a list per layer, `gated` or `gating_mode` none / gated / blended with a `secondary_activation` (Sigmoid by default; the trainer writes `null` entries for ungated layers), `head_size` + `head_bias` or a `head` object with its own kernel size and dilation, `layer1x1` / `head1x1` on or off, a post-stack `head` and `head_scale`), LSTM (`num_layers`, `hidden_size`; the initial states are part of the weights), Linear (an impulse response, up to 16384 taps) and ConvNet (kernel-2 blocks with batchnorm folded into a scale and a shift). **`SlimmableContainer`** files (what ToneHunt's newest "A2" captures are: `config.submodels`, each a whole capture with a `max_value`) read as their biggest size, the lighter ones kept as `Model::variants`; the container's own metadata (name, loudness) wins over the sizes'. The A2 shape itself (23 layers, kernel sizes 6 and 15, LeakyReLU, a 16-tap head) is an ordinary WaveNet to the engine: full 8 channels ~57 ms per second of audio, light 3 channels ~17 ms. Activations: Tanh (Fasttanh maps to it), Hardtanh, ReLU, LeakyReLU, PReLU, Sigmoid, SiLU, Hardswish, Softsign, LeakyHardtanh. Refused with a plain message: FiLM conditioning, a `condition_dsp`, grouped convs that don't fit, more than one channel in or out, `Sequential` files, a container inside a container, layers over 128 channels, convs over 32 taps.
- **Weight order** (documented at the top of `Nam.cpp`): Conv1D / Conv1x1 take, per group, per output, per input, per tap, then the biases; tap `k` reads `(kernel - 1 - k) * dilation` samples back. A WaveNet layer array is its rechannel 1x1, then per layer the dilated conv (bias), the condition's 1x1, the layer 1x1 (bias) and the head 1x1 (bias) when active, then the head rechannel; after the arrays the post-stack head and one float, `head_scale`. LSTM: per layer W (4·hidden rows by input + hidden columns, row-major, gates i f g o), bias, h0, c0, then the head's weights and bias. Linear: the impulse response (`ir[0]` on the current sample) then the bias. ConvNet: per block the conv, then batchnorm's mean, variance, weight, bias and eps; then the head.
- **Running it.** Weights are stored input-major (`w[(tap * in + i) * out + o]`) and a `Channel` runs a layer over a whole chunk of up to 64 frames at a time with the frames innermost: every convolution keeps a channel-major *mirrored* history (each frame written twice, so any run back from the newest frame is one contiguous stretch), and the inner loops are plain axpys over 64 floats, no reductions, so they vectorise without fast-math. tanh is the usual rational approximation (good to ~1e-7). The hot loops are compiled twice with GCC's `target_clones` (AVX2 + FMA, and the baseline) and picked at load time, so a generic .deb runs a standard capture at ~85 ms per second of audio per channel on the user's Ryzen (the reference core, Eigen, generic build: ~190 ms). Nothing allocates after construction.
- **A device's `Runtime`** holds two `Channel`s and is built, prewarmed (the receptive field of silence, half a second for an LSTM) and cached by the engine on the UI thread (`Engine::publish`, `m_namCache` by device id, reused while the capture and the rate stay) and handed to the processor through `Context::nam`; the audio thread only runs it. The two sides stay **linked** (one network) while the two channels carry the same signal, as a mono guitar input does; when they first differ the right side copies the left's history and runs its own network from then on. `panic()` restores the prewarmed state (a copy kept at construction) on the next block.
- **Sample rate.** Captures are trained at one rate (48 kHz for nearly all). When the engine's rate differs (within 4x either way), the sound is resampled to the capture's rate before the network and back after it: a 32-tap windowed-sinc interpolator with 64 sub-sample phases (`Runtime::Resampler`), cut below the lower Nyquist, and a small FIFO that hands out exactly n frames a block; `Runtime::latency()` says the delay (39 frames at 44.1 kHz). A Linear low-pass designed at 48 kHz measures the same (within 0.01 dB up to 4 kHz) at 44.1 kHz.

### The device (`DeviceType::Nam`, `namdev::` indices, `Guitar.cpp`'s neighbour in `Nam.cpp`)

- **"NAM"**, an audio effect in the browser's Guitar category next to the Guitar Amp (and in Audio Effects). Parameters, appended only: Input (-24 .. +24 dB before the capture, the drive), Output (-24 .. +12 dB after it), Mix, Gate (a noise gate before the capture: -90 .. -30 dB, all the way down is off, 6 dB of hysteresis), Normalize (the capture's loudness metadata to -18 dB, as the NAM plugin does; nothing without the metadata), IR (the cabinet slot on / off), IR Mix, Quality (Full / Light: which size of a container runs; `Engine::publish` picks the variant, so the runtime is rebuilt when it changes; a single-size capture ignores it). A 5 Hz DC blocker follows the network (captures can carry an offset). The IR slot is the amp's Loaded IR: `Device::samplePath` / `sample` through `Context::ir` and a `Convolver` per side (one block of latency). `meter[0]` / `[1]`: in / out peaks.
- Device data: `Device::modelPath` (absolute; saved as `"model"` with a `relativePath` like samples, found again after the set's folder moves) and `Device::model` (null while the file is missing or unreadable: the device plays dry and the panel says so in red, `theme::devMissing`). Presets keep the path; Collect All and Save copies captures next to the set like samples.
- **Panel** (`GuitarBodies.cpp`, `NamBody`): the capture view (name, architecture summary with the sizes a container holds, gear, who modeled it, loudness / rate / weight count; or "Missing: file" in red), Load… (a .nam chooser) and Clear, the knobs, Normalize with the loudness read-out, Quality with the sizes' channel counts, an in / out meter with a dB scale, the cabinet IR view with Load IR… / Clear, IR and IR Mix. A `.nam` dropped anywhere on the panel loads the capture, a WAV the IR; every control has Info View help and a `mapTarget`.
- **Browser and drops.** Sample folders list `.nam` files among the samples (glyph ≈, payload `path|n`), and the User Library lists any `.nam` kept in the presets folder; `Browser::modelFilesIn` is `audioFilesIn`'s sibling. A capture drags as a device (`devicedrag::Payload::Model`): dropped on an audio track, a return, a rack chain or the Device View it lands as a NAM device holding it; on the empty space it makes an audio track named after it; double-clicked it goes on the selected track; an unreadable file is refused with the reason. `nam::writeExample` writes a small capture with random weights for screenshots and tests.

### Tests (52 new; 630 in all)

- The reader: a Linear capture against its definition (taps, bias, name, loudness, rate; the exact FIR); the wrong weight count, an unknown architecture, junk and a missing file refused. A container of two sizes (the full one read, the light one a variant, the container's metadata on top, the light size running as its own network: the same weights at a fifth of the head scale give a fifth of the output); the A2 layer shape with the weight counts real files carry (1871 light, 12146 full), its receptive field, finite output; Quality Light through the engine runs the light size. A tiny gated WaveNet and a tiny LSTM with deterministic weights against references written out in the test from the format (every layer kept for the whole signal; the cell equations), sample for sample. A standard 16 / 8 WaveNet with random weights: 13802 weights, receptive field 4093, finite, bounded, timed (printed as ms per second of audio) and under budget; a stereo input parts the sides. The example writer. Input / Output / Mix / Gate (closes on a quiet passage, off at the bottom) / Normalize (-12 dB loudness halves the level); dry without a capture; the IR slot (convolution one block late, IR Mix 0 and IR off leave it alone). Resampling: a 48 kHz low-pass capture measures the same at 44.1 kHz. The engine: an audio track monitoring Input 1 plays through the capture, dry when it's missing. Set file (relative path, found after the folder moves, round trip, a missing capture keeps its path), presets, the browser listing and `modelFilesIn`, drags (parse, drop on a track, on the empty space, refused when unreadable), the panel (every parameter has a control, Load…, the three displays, fits the height, a dropped .nam and a dropped WAV each one undo step, undo, a missing capture).
- `--screenshots` adds `29-nam-capture` (a guitar track with the Tuner and a NAM holding the example capture and a cabinet IR); `devices/` has the NAM panel (every panel after it is renumbered).

### Known gaps after M15

- Two newer WaveNet features aren't read yet: FiLM conditioning and a condition DSP (the core's feature-test models use them; no ToneHunt capture seen so far does); stereo captures; grouped convs are stored block-diagonally (correct, not fast). Containers always offer their lightest size as Light (a three-size container has a middle size nobody can pick).
- No latency compensation: the IR adds a block, resampling ~40 frames (only when the engine runs off the capture's rate; Sessions runs at 48 kHz, so today it only happens for captures trained at another rate).
- The gate is a simple one (fixed attack / release); no "Calibrate input" (the capture's `input_level_dbu`) since Sessions doesn't know the interface's dBu-to-dBFS.
- Captures load synchronously on the UI thread (a 13802-weight file parses and prewarms in under 10 ms; a 1 MB A2 file would take a few tens of ms).

## Milestone 16: racks, completed (done)

Asked for: "continue iteration the next milestone". From the M16 "next" list this pass took the rack items, the longest-deferred ones (on every "next" list since M8): Drum Rack pads holding instruments and effects, a MIDI Effect Rack, key / velocity zones and a chain selector. It also did the chain housekeeping M9 left: moving chains, and deleting one no longer leaves the later chains' automation and mappings behind. The NAM follow-ups, I/O routing, multi-clip editing, Complex warp and packaging move to Milestone 17.

### Zones and the Chain selector (`Model.h` `Zone` / `ChainZones`, `Device::zones`, `racks::ChainSelect`)

- A `Zone` is `lo .. hi` (0 .. 127) with `fadeLo` / `fadeHi` steps inside either end. `weight(x)` is 0 outside, 1 inside, and linear in the fades (`(x - lo + 1) / (fadeLo + 1)`); `sane()` clamps and orders. `ChainZones` holds a chain's `key`, `vel` and `select` zones. The defaults (every key, velocities 1 .. 127, selector 0 .. 0) change nothing while the selector sits at its default 0. `Device::zones` runs parallel to `Device::chains`; `appendChain`, `deleteChain`, `moveChain` and `normalizeRacks` keep them the same length. Undo compares them (`operator==` on Device).
- Every rack gets one appended parameter, **Chain** (`racks::ChainSelect` = 40, whole 0 .. 127, default 0, `racks::kParams` = 41). It is automatable, mappable, and a macro target ("Chain Selector" in the rack macro menu, labelled "Chain Sel").
- Engine (`chainNotes` in `Engine.cpp`): an Instrument or MIDI Effect Rack chain gets the note-ons that are inside all three of its zones: pitch in the key zone, velocity (as 1 .. 127) in the velocity zone, and the selector in the chain zone. The three weights multiply and scale the note's velocity, so fades play softer. Releases (`noteOff`, `allOff`) always pass, so when the selector leaves a chain, its held notes ring and end normally. An Audio Effect Rack chain's input is scaled by its chain-zone weight at the selector (`ChainMixer::scale`, gliding ~10 ms), so tails ring out and moving the selector crossfades.
- `Project::setChainZone(track, rack, chain, which, zone, mergeKey)` is one undo step. Its merge keys are `deviceMergeKey(rack, 232 + chain)`; 232 .. 239 were free between the display gestures and the macro ranges. Set file: racks get `"zones": [{"key": [lo, hi, fadeLo, fadeHi], "velocity": […], "select": […]}, …]`, written only when some chain's zones aren't the defaults and read only for rack types.

### The MIDI Effect Rack (`DeviceType::MidiEffectRack`, kind MidiEffect)

- It has the same 41 parameters as the other racks, but a chain's first two knobs are **Transpose** (-48 .. +48 st) and **Velocity** (0 .. 200 %, a scale) instead of Volume and Pan. `racks::Transpose` / `racks::VelocityScale` alias the same indices, and `rackParams(true)` names them. `resetChainKnobs` sets a new chain's knobs from the spec defaults; the old literal 0.75 for Volume would have meant +12 st here.
- Engine (`runMidiRack`, `rackType` 3): each chain gets `chainNotes` of the incoming notes (transposed, velocity scaled; none when the chain is off or not soloed) and runs its own MIDI effects. The chains' notes are merged into `m_rackEv`, sorted, with a release that two chains send at the same offset kept once, and they replace the track's notes. Switched off, the rack passes notes through untouched.
- Its chains take MIDI effects only (`chainAccepts`, `normalizeRacks`). **Group** now accepts MIDI effects: MIDI effects alone make a MIDI Effect Rack, with an instrument they make an Instrument Rack, and MIDI plus audio effects without an instrument are refused. The panel's chain rows show Transpose and Velocity and no meter. The browser lists it under MIDI Effects.
- Sounds: **Octave Layers** (the played notes, an octave up at 70 %, an octave down off; Macros 1 / 2 switch the layers) and **Split Arp** (an Arpeggiator on the notes below C3). `RackRecipe::Chain` gained `keyLo` / `keyHi`, `transpose` and `velocity`. **Bass and Keys Split** is an Instrument Rack recipe split at G3.

### Drum Rack pads holding chains (`holdsChains`, `drumrack::kPlayNote`)

- A Drum Rack straight on a track (`rack == 0`) holds one chain per pad, like a rack. Devices with `Device::rack` = its id and `Device::chain` = the pad follow it in the track's list, pad by pad; each pad's order is MIDI effects → instrument → audio effects.
- `holdsChains(d)` (the racks, plus top-level Drum Racks) replaced `isRack` wherever blocks are walked: `deviceBlockEnd`, `normalizeRacks`, `deviceBlocks`, `placeBlocks`, the Device View. `isRack` still means the three racks with macros and chain lists.
- A Drum Rack inside a rack's chain stays a plain sample player: it holds nothing, and `normalizeRacks` leaves it in its chain. A Drum Rack whose pads hold devices can't go into a rack's chain or be grouped. Nothing that holds chains goes on a pad, and neither do Macros or another Drum Rack.
- Placing uses `DevicePlace{0, drumId, pad}` (pads 0 .. 15; it never makes a new chain), so the browser, presets and device drags all go through `insertDevices` / `moveDevices` / `devicedrag::drop` unchanged. `deleteChain(drum, pad)` is **Remove Pad Devices**: the pad and its sample stay. Moving, copying, duplicating the track and presets take the pads' devices along through the existing block code.
- Engine (`runDrumRack`, `rackType` 4; `publish` fills `DeviceSlot::padHas` / `padInst` bit masks):
  - One `DrumRack::render` call plays the pads without devices and starts every sample voice. `Context::padSample` leaves out pads with an instrument; their hits still choke and light the pad. `Context::padPlay` holds back the voices of pads that have effects.
  - Each pad with devices then renders into `m_chainL/R`. A pad's instrument gets the pad's hits as C3 + the pad's Transpose, plus an `allOff` when another sounding pad of its choke group is hit. A pad without an instrument gets a second `render` call with `padPlay` = that pad; its sample voices wait (`Voice::delay`) until the frame they were hit on.
  - Then the pad's devices run, and the Drum Rack's `ChainMixer` mixes the pad in. An instrument is mixed at the rack's Volume × the pad's Volume and Pan; a sample's voice has them already.
  - Switched off, the rack fades out like an instrument and its pads' devices get `panic`.
- `ChainMixer` (`Devices.h`) is what `RackProcessor` used to do inline: gliding gains and per-chain peaks, and now `scale` too. Racks and the Drum Rack expose theirs through `DeviceProcessor::chainMixer()`, which `Engine::takeChainPeak` uses.
- Panel:
  - The pads take device drops (`RackPads::takes`) as well as samples. A pad with an instrument shows its name in cyan, and a dot marks a pad with effects. Right-click → Clear Sample / Remove Pad Devices. `PadWave` says "E1 plays Sub Bass at C3" for an instrument pad.
  - The Device View shows a Drum Rack in a `RackPanel` with the selected pad's devices and a drop zone ("Drop an instrument or effects onto pad E1 Tom Low"). `selectDrumPad` now announces through `rackSignals()` (only when the pad changes) so the view follows.
  - The piano roll names an instrument pad's row after the instrument.
  - Helpers: `devui::drumPadLabel`, `padDevices`, `padInstrument`.

### Chains: moving, deleting, the panel (`Model.cpp` `remapChainKnobs`, `ui/devices/RackBodies.cpp`)

- `Project::moveChain(track, rack, from, to)` (one undo step) and `deleteChain` both go through `remapChainKnobs`. Everything on the chain knobs follows its chain: their values, the track's automation and its clips' envelopes, overridden targets, and macro mappings (the rack's own and Macros devices'). A deleted chain's go, and so do its devices' automation and envelopes. MIDI Map / Key Map mappings are not remapped (they aren't undoable).
- The rack panel's header has **Key / Vel / Chain** mode buttons (while the editor is shown), the **Chain** selector field, **Zones** and **+**. **Zones** (`devui::showZones`, per rack, not saved) widens the body by 223 px and ends each chain row in a `ZoneBar`, under a `ZoneRuler`:
  - Drag an end in the lower half to set the range, the upper half at an end to set the fade, the middle to move the whole zone. A click outside the zone moves the nearer end there; double-click restores the default.
  - The ruler shows note names, velocities or selector positions. In Chain mode, a click or drag on it moves the selector.
- Drag a chain's name up or down to move the chain; an accent line shows where it will land. A click without a drag still shows the chain. That now happens on release, so a drag survives the rebuild. Checked buttons are lit in `theme::accent`.

### Tests (71 new; 701 in all)

- Zones: weights, fades, `sane()`, the defaults; the appended Chain parameter and the MIDI chain knobs' units.
- Sound, each compared against references rendered from the plain synths:
  - Racks: default zones change nothing; a key split; velocity layers; the selector picking chains; a key fade playing softer; an Audio Effect Rack's selector and its fade (1.5× the sound half way in).
  - MIDI Effect Rack: an octave layer (= the four notes played straight); a chain off, a chain not soloed, the rack off; Velocity 50 %; key zones with a transposed chain; a chain's own Scale effect.
  - Pads: an instrument on C1 (= the snare alone + the synth at C3); the pad's Transpose, Volume and Pan; a pass-through Utility on a sample pad changing nothing (the delayed voices land on the right frame); a muted Utility silencing just that pad; effects after a pad's instrument; the rack off; a choke group releasing a pad's instrument.
- Editing:
  - Pads: devices onto pads (order, one instrument per pad, what's refused); the Drum Rack moving, undoing and duplicating with its pads; Remove Pad Devices; no grouping; deleting the rack; `normalizeRacks` on a scrambled list and on a Drum Rack inside a rack; a preset.
  - MIDI Effect Rack: grouping MIDI effects, its chain rules and new-chain defaults, MIDI tracks only, the three recipes.
  - Zones and chains: a zone edit is one undo step, drags merge, zones stay sane, undo. Delete Chain and Move Chain carry names, zones, knobs, automation, clip envelopes and macro mappings, and undo restores all of it.
  - Set file: a round trip with zones, a MIDI rack and pad devices; `"zones"` written only when needed.
- Panels: the zone bars and ruler (and the rack still fitting the device height); dragging a bar's end (one undo step); double-click; the selector from the ruler; dragging a chain name; the MIDI rack's fields; a Drum Rack in its frame; an instrument dropped on a pad (one undo step, selected, shown in the Device View); picking another pad.
- Two older tests changed. The device count is now 47 (five MIDI effects). The side-by-side check of a long chain now compares positions in the view's coordinates, because a Drum Rack's panel sits inside its rack frame.
- `--screenshots` adds `30-drum-rack-pad-chains` and `31-rack-zones`; `devices/` adds `47-midi-effect-rack`. Every rack panel capture changed (the new header).

### Known gaps after M16

- Key and velocity fades scale the note's velocity, so on velocity-sensitive instruments a fade changes the tone as well as the level (rather than the volume). An Instrument Rack's chain-select fade does the same; only the Audio Effect Rack's is a true gain.
- Pads: no per-pad sends, mute / solo, or "Play" note other than C3 + Transpose. A pad's instrument has no choke of its own beyond the release its choke group sends; a sample pad in the group is choked by its hits. Pads can't be dragged onto each other (swap / copy pads).
- A sample pad with effects starts its voices on the exact frame. But a choke that reaches it from another pad in the same block starts fading at the block's start, up to 128 frames early.
- Chains can't be dragged between racks, and racks don't nest (no racks in chains, no Drum Rack on a pad). MIDI Map mappings to a rack's chain knobs don't follow Move / Delete Chain.
- The zone editor has no keyboard entry of values and no "auto-select" (selecting the chain that plays a key). Zones are hidden by default, and that setting isn't saved.

## Milestone 17: after the 0.16 hands-on (done)

The user's notes after playing 0.16, taken one by one, and the Monolith iterated further ("think of Massive, and how versatile it is"). The rest of the old next list (NAM internals, routing, multi-clip editing, Complex warp, packaging) moves to Milestone 18.

### Session first (`Transport::launch`, `Command::SessionTakeover`)

- Reported: with the song stopped, playing live and launching a drum clip from the Session View started the Arrangement as well. That was the old rule (launching starts the transport, and tracks without a Session clip play the timeline), but the user wants the Session View to be the live surface: "the arrangement should not interfere here at all".
- Now: a launch (or a scene, or a slot recording) from a standstill marks every track overridden (`Track::overridden`, and `Command::SessionTakeover` sets every `PlayState::overridden` in the engine) before `play()`, so the Arrangement stays silent for all tracks until **Back to Arrangement** (which already un-overrides everything). A launch while the song plays overrides only its own track, as before. The orange Back to Arrangement button therefore lights after a standstill launch, which is the way back.

### Panes (`MainWindow`)

- The browser sits in a horizontal `QSplitter` (`browserSplit`, a 5 px handle like the Session View's) beside the main views; `Browser` lost its fixed 292 px width (minimum 220). The width is saved (`window/browserWidth`); View → Browser still hides it.
- The detail pane (`m_bottom`): `fitDetailPane(clip)`. Showing the Device View sets the pane to `devicePaneHeight()` (the header, the chain's margins, a rack frame, a panel's title and body, a horizontal scroll bar) with `setFixedHeight` and disables the splitter handle; showing the Clip View frees it again and restores the height you dragged it to (`m_clipPaneHeight`, saved as `window/clipPane`, falling back to the old `window/split`). `showClipView` remembers the Clip View's height whenever it leaves it.

### Clip clocks (`ClipGrid`)

- `loopFraction(track, clip)`: where a playing clip is in its loop from the beat you hear, clamped at 0 when the engine's start sits ahead of the heard beat (output latency) and wrapped for one-shots. `trackStatus` uses it.
- A playing slot draws a 2.5 px rounded bar along its bottom and a 10 px clock at its right (the name elides to make room), both in the track colour darkened (recording: ink). The stop row's clock is a ring in the track's colour that fills once per loop, the pass count beside it in the same colour. Repainting was already per transport tick (16 ms) while anything plays; the previous grey pie and the 2 px translucent bar were what read as glitchy.

### NAM ◀ ▶ (`core/Import.*` `captureLibrary` / `stepCapture` / `captureIndex`, `Mapping.cpp`, `GuitarBodies.cpp`)

- `import::captureLibrary()`: every `.nam` under the User Library folder and the browser's sample folders (QSettings `browser/folders`), subfolders included (5000 files per folder at most), sorted case-insensitively by path. `stepCapture(p, track, device, dir)` loads the next / previous one, wrapping, skipping files `nam::load` refuses; a capture that isn't in the library steps through its own folder's `.nam` files; no capture loaded starts at the library's first (or last, going back); a lone capture says so. It emits `structureChanged` / `deviceChanged` and expects the caller's EditScope.
- The panel has ◀ ▶ before Load…, and a second row with the file name and "3 / 42" (or "not in the library"); the library is scanned only when the capture changes (`m_placePath`), never per refresh.
- Mapping targets `device:<track>:<device>:next` / `:prev` (buttons: `isContinuous` false, `targetName` "NAM · Next Capture", `Mapper::trigger` steps inside an EditScope). The buttons carry them as `mapTarget`, so Ctrl+M maps a pedal to them.

### Monolith, second pass (`DeviceSpecs.h` `monolith::`, `Monolith.cpp`, `InstrumentBodies.cpp` `MonolithBody`)

- Eight appended parameters (`kParams` 112 → 120; older sets get the defaults): **Bend Range** (0 .. 24 st, 2), **Restart** (off), **LFO1 / LFO2 Retrig** (off), **LFO1 / LFO2 Fade** (0.001 .. 5 s, off), **Step1 / Step2 Retrig** (off); `lretrig(l)`, `lfade(l)`, `sretrig(s)`; `sectionName` knows them. Three appended sources: **Wheel** (0 .. 1), **Pressure** (0 .. 1), **Bend** (-1 .. 1); `kSources` 18.
- Processor: every voice keeps `startBeat` / `startClock`, its own LFO phases and stepper outputs. `voiceSources(v)` (in `modulate`, every kSub samples) picks the shared LFO / stepper or, with Retrig, the voice's own counted from its start (synced rates against the beat since the note, free rates from a per-voice phase), and scales an LFO by `min(1, age / Fade)`. `stepValue()` is the stepper math shared by the global and per-voice paths. The pitch bend (`Context::bend × Bend Range`) is added to every oscillator's and the mod oscillator's pitch in `modulate`. Restart sets every oscillator phase to 0 on a fresh voice instead of random. Voices 1 .. 7 now seed their noise generators differently (voice 0 keeps the old seed, so M13's deterministic tests still hold): before, two voices starting together had the same "random" phases.
- The keyboard's controls reach the engine as `Command::Perform` (a = 0 bend / 1 wheel / 2 pressure, b = value) from `Transport::perform(track, kind, value)`; the engine keeps them per track id (`m_perf`, by id so inserting tracks can't misplace them) and copies them into the track's `Context` (`bend`, `wheel`, `pressure`) before `runChain`, so every instrument on the track can read them (only Monolith does so far). `MidiInput` emits `pitchBend(channel, 0 .. 16383)` and `channelPressure(channel, 0 .. 127)` (0xE0 / 0xD0, and ALSA's PITCHBEND / CHANPRESS events); `MainWindow` sends them, and CC 1 when it isn't MIDI-mapped, to `keyboardTargets()` (the armed MIDI tracks, else the selected one), the same tracks the keys play.
- Panel: two pages on page buttons (`monoPage`: "Synth", "Mod"; `monoPages()` remembers the page per device). Synth (~1000 px): three oscillator columns (table, wave, warp mode, Pos / Intens / Level, Pitch / Fine / F1-F2), Mod Osc + Noise · Feedback, Filter 1 (with Par/Ser) and Filter 2 (with Key, F Mix and the Insert row), Voice (mode, unison, Detune / Spread / Glide, Bend / Vel>Amp / Volume, Restart), Macros. Mod: the four envelopes in a row over the two LFOs (rate, display, shape, Free Hz, Fade, Retrig) and two steppers (rate, display, Steps, Glide, Retrig), the 18 source chips, the routes list (206 px, elided labels with tooltips, a hint when empty). The body is 1107 px wide; the Device View scrolls. Dropping a source on a knob opens the Mod page.

### Tests (34 new; 735 in all)

- Session: a launch from a standstill overrides every track, the Arrangement track stays silent while the clip plays, Back to Arrangement brings the timeline back, a launch while playing overrides only its track, a scene launch does the same as a slot.
- Window: the browser in its splitter and wider when the divider moves; the Device View's pane fixed (min == max, the handle disabled, every panel on screen); the Clip View's pane draggable and returning at its dragged height.
- Clocks: `trackStatus` fractions and pass counts from the heard beat (never negative), a one-shot's clock stays full, the grid paints.
- NAM: the library from both folder kinds (sorted, subfolders), `captureIndex`, ▶ skipping an unreadable file, ◀, wrapping, a capture outside the library stepping through its own folder, no capture loaded, a lone capture, the mapping targets (names, buttons, one undo step, undo), the panel's buttons and "n / N".
- Monolith (each run deterministic through Restart): bend at 12 st = the note an octave up and half a bend = a tritone; the wheel as a source (and nothing at 0); LFO Retrig (a note 0.3 s later matches one at the start sample for sample; off, it doesn't); LFO Fade (a 5 s fade barely moves the cutoff); Step Retrig (the sequence from step 1 against the song's beat); Restart (two hits alike; off, different). The whole way: `Transport::perform` to the track's Monolith only. `MidiInput` signals for pitch bend, pressure, CC 1.
- Changed: M13's panel test expects two pages and a width up to 1140 (its message now prints the size); M13's knob count is 120 with the appended ones; the Monolith's six-page screenshots are gone (`devices/12-monolith.png`, `12-monolith-mod.png`), `23-monolith-synth` shows the Synth page and `23-monolith-mod` the Mod page.

### Known gaps after M17

- Only Monolith reads the performance controls; Analog, FM, Wavetable, Sampler, Organ, Bass and Electric ignore pitch bend, the wheel and pressure (each voice fixes its frequency at note-on). Polyphonic aftertouch isn't parsed. The controls aren't recorded into clips (no clip envelopes for them yet).
- A Session clip launched from a standstill starts the transport at the start marker, so the Arrangement position moves there (silent) and Back to Arrangement resumes from wherever the playhead has reached.
- The Device View's fixed pane height is computed, not measured: a device body taller than `devui::kBodyHeight` would be clipped (none is; the panel test checks every device).
- The NAM library scan is synchronous (a few thousand files is fine); the "n / N" count ignores unreadable files.
- Monolith: unison stays at 4 voices (the Unison choice list can't grow without remapping saved positions); no per-voice LFO phase offset or LFO one-shot (envelope) mode; the Synth page is wider than a 1024 px Device View.

## Milestone 18: everything in sync, and the pedals revisited (done)

Asked for after playing 0.17: "the guitar pedals are a little underwhelming and sound very similar and kind of bad … keep the current device design, modern and minimalistic with specialized graphs and buttons … a deep pass on each pedal", and "everything needs to be in sync: project tempo is master, every clip adjusts to that tempo, warped to match the closest tempo that's in sync (project 172, a sample of 87 → 86), each clip has a button to turn the warp off (on by default), Sessions should detect what BPM a sample has".

### Tempo detection (`core/Tempo.h/.cpp`, `tempo::detect`)

- An onset strength curve at ~187 hops a second (hop = rate / 187.5): three bands (one-poles at 160 Hz and 2 kHz), each hop's level as the fourth root of its mean square (loud hits count more than quiet ones, but not by their whole power: a log flux let the hats on every eighth drown the beat), the rise since the last hop (falls count nothing), bands weighted 1 / 0.8 / 0.6. The first hop sets the levels (its jump from nothing isn't an onset). Smoothed over three hops, a one-second trailing mean taken off, floored at 0.
- The autocovariance (the mean taken out) over lags from 240 down to 40 BPM, and up to four times that for the comb. A candidate lag must be a peak of its own (not the slope down from lag 0), scores `r[L] + 0.5 r[2L] + 0.2 r[3L] + 0.25 r[4L]` (a real beat repeats at two, three and four beats; a lag of three eighths doesn't) times a gentle prior two octaves wide around 110 BPM. The winner must be strong (≥ 0.15 of lag 0), tower over the other lags (3 sd above their mean) and repeat at its double (≥ 0.35 of itself), else there's no tempo (noise, a hit, a word; under a second of audio or fewer than three hits returns none at once). The period is refined with a parabola through the neighbouring lags.
- The octave: among the half, the found and the double, a loop (the file's length a whole number of bars within 3 %) wins, then the nearest to 100 BPM. For sync the octave doesn't matter (see below); it's what the Clip View shows as found.
- `SESSIONS_TEMPO_DEBUG=1` prints the top lags with their multiples' correlations to stderr: the way to tune it on a file that's found wrong.
- Checked on synthetic drum loops (kicks on the beats, a snare on 2 and 4, hats on the eighths) at 87, 96, 128 and 172 BPM (found within 1 %, or at their half), at 44.1 kHz, on a loop cut short of its bars, a phrase, white noise and a single hit.

### Matching the set's tempo (`tempo::octaveFor`, `tempo::syncTo`, `Clip::detectedBpm`, `Project::newAudioClip` / `setTempo` / `syncClipToTempo`)

- The set's tempo is the master. `octaveFor(sampleBpm, setTempo)` is the power of two that brings the set's tempo nearest the sample's (87 at 172: one up, so the sample plays at 86 with two set beats per sample beat; 87 at 120: none, it plays at 120; at 60: one down). A new audio clip (`newAudioClip`, every drop and import) runs `syncTo`: the found tempo, the sample's beats at it, times 2^k; a whole number of bars within 8 % (a loop cut a little short or long is stretched to them) or of beats within 0.04 loops, anything else plays once, in tempo. No tempo found: the old guess (a power of two bars within 20 % at 60–200 BPM), else the file at its own speed, once. `Clip::detectedBpm` keeps what was found (saved as `"detectedBpm"`, in `operator==`); `sampleBpm()` is that times 2^k, so the Clip View's BPM field still reads (174 for the 87 loop at 172) and `:2` / `x2` still work.
- `Project::setTempo` re-matches: every audio clip in the set (slots and timeline, each once: `audioClips()`) with Warp on, a found tempo and no takes, whose nearest power of two changed, is rescaled by 2^Δk (`rescaleClip`: warp markers around the first, length, envelopes, comp regions, and every placement's length and offset, so a placement keeps showing the same audio). The callers' EditScope makes it one undo step with the tempo. Dragging the tempo across a boundary (87 × √2 ≈ 123 for an 87 BPM loop) flips the clip's speed: that's the rule the user asked for. Engine tests that want the old half-speed behaviour pin their clip with `detectedBpm = 0`.
- `Project::syncClipToTempo(clip)` (one undo step): finds the tempo when the clip has none yet, then sets the markers, length and loop from the audio; placements and envelopes scale with it. The Clip View's Sample box gained a **Tempo** row: "found 87.0 BPM · plays at 86.0 (1/2 of the set's tempo)" (or "Warp off: plays as recorded", or "no steady beat found") and a **Sync** button (`syncTempo`). **Warp** (`warpOn`) was already there and on by default.

### The pedals (`audio/Guitar.cpp`, `ui/devices/GuitarBodies.cpp`, `DeviceSpecs.cpp`)

- Seven drive pedals, each its own circuit instead of one clipper behind different filters, all still 4x oversampled, with an envelope of the input per channel for sag and gating; `gainOf` and `transfer` (`audio::pedalTransfer`) are static so the panel draws the very clipper. Overdrive: the input plus 0.55 × the clipped mids (the diodes in the feedback loop: the lows pass at gain 1), Tone a 600 Hz .. 7.5 kHz low-pass, appended **Clip** (808 / Asym: the negative side 0.72 for even harmonics). Distortion: a treble-leaning gain stage (+2 .. +10 dB shelf at 1.5 kHz, the high-pass rising with the gain), the op-amp's slew, appended **Clip** (Silicon hard knee / LED a 1.7 ceiling, louder / Germanium 0.55 × tanh(u / 2.2), quieter and compressed), the Filter a one-pole after. Metal Distortion: Tight, a pre-emphasis peak at 1 kHz, tanh → a coupling cap at 4x → hard diodes → an interstage 6 kHz cut, then Low / Mid (sweepable) / High at ±15 dB. Chainsaw: lopsided hard diodes, then the 92 Hz (−10 .. +18 dB) and 1.1 kHz (−10 .. +16) gyrator peaks with a 5 kHz shelf. Metal Boost: gain 1 .. 9 into a soft tanh, Tone a ±8 dB treble shelf, unity near Level 7. Fuzz: a dark input (4 kHz), tanh → a coupling cap → `fuzzClip` with appended **Bias** (−0.35 .. +0.1; starved, the output is gated below 0.03 × (1 − bias)² of full scale with a smooth step), the muff tone blend, a 5.5 kHz output. Crunch: two triode stages (`tube`) with the first's bias following the envelope (0.22 .. 0.37: more even harmonics on the attacks), Bright a +7 dB shelf before, Tone a tilt, appended **Presence** (3.5 kHz, −4 .. +8 dB).
- Twang: three springs per side (12 all-passes each, lengths wobbling ±0.6 samples at 0.37 / 0.48 Hz), feedback 0.62 .. 0.92 with Dwell, tanh in the loop. Phase Pedal: appended **Depth**, the sweep 175 Hz .. 2.8 kHz at full depth, meter[2] the stages' coefficient for the panel (`audio::phaserResponseDb`). Chorus Pedal: appended **Mix** (50 % the chorus, 100 % a vibrato), pre-emphasis and a gentle squash into the bucket brigade, 2 .. 10 ms. Wah: the peak sharpens (Q × 0.7 .. 1.3) and rises toward the toe; the lows pass at −14 dB.
- Levels at the defaults sit within 6 dB of the dry sound for every pedal (the M10 check); the seven drive pedals are told apart by brightness, symmetry and crest factor at their defaults (`pedal voices` is printed by the test).
- Panels: `PedalBody` is a device like the others now: a column of knobs (and the mode strips / toggles), then the graphs: `TransferCurve` (one cycle of a quiet note, faint, and what the clipper makes of it at this gain, with the small-signal gain as a caption), `ToneCurve` (the filters' response, ±24 dB), `PhaserCurve` (the notches where the sweep is, live), `RateLight` (the sweep's shape and a light), `SpringTank`, `WahRocker` (drag it; `mapTarget` for an expression pedal). The stompbox faces, lettering and footswitch are gone (the title bar's activator switches a pedal); the hardware colours went with them, so Light and Mid themes get the device palette. Width ≤ 540 px, graphs 124 px tall.

### Tests (32 new; 767 in all)

- Tempo: four synthetic loops found within 1 % (or their half) and at 44.1 kHz; noise and a hit have none; `octaveFor`; a 4-bar 87 BPM loop is 32 / 16 / 8 beats at 172 / 120 / 60; a hit plays once; a loop 0.1 s short is stretched to its bars, a phrase plays once in tempo. Clips: the 87 loop in a 172 set (32 beats, found 87, 174 as its own BPM); `setTempo` to 100 re-matches to 16 with the placement and envelope scaled, one undo step, back at 172 it's 32, 150 changes nothing, undo takes both back, Warp off is left alone; Sync after hand-moved markers; Sync on a one-shot; the set file keeps `detectedBpm`; the engine plays the 87 loop's beats on every second beat of the 172 set; the Clip View's Tempo row, Warp and Sync.
- Pedals: the seven drive pedals apart at their defaults; the overdrive's clean lows and mid hump, 808 vs Asym on a sine, clean to driven by a THD measure; the rat's three pairs (LED louder, germanium quieter and no brighter) and Filter; the MT-2's mid swing and Tight; the boost's cleanliness and Tight; the fuzz's bias gate and tone; Crunch's Bright and Presence; the phaser's sweep width by Depth and its notch curve; the chorus's Mix 0 dry, Mix 100 % vibrato at the same level, Stereo parting the sides; Twang dry at Mix 0 and ringing after the playing stops; every clipper curve bounded, rising, near 0 at rest; all eleven panels with graphs, fitting, ≤ 540 px.
- Changed: M4's "near a bar" clip test prints what it found; two M4 engine tests pin their clip (`detectedBpm = 0`) since a tempo change now re-matches; M10's footswitch test became "a pedal's panel has its graphs and fits"; the pedal device captures (`devices/30-40`) changed completely.

### Known gaps after M18

- Tempo detection is autocorrelation-based: no downbeat (clips start at the file's start), no tempo changes within a file, and a swung or sparse part (a bass line, a pad) may come back with none or with a half / double that the loop-roundness rule can't settle; tested on synthetic loops only, so far. Detection runs synchronously on the UI thread (a minute of audio in a few tens of ms; 90 s analysed at most).
- Re-matching on a tempo change moves placements' lengths in the Arrangement (so the audio stays the same); neighbours aren't moved, so a grown placement can run under the next one.
- The pedals' circuits are shaped by ear and by the checks above, not against hardware recordings; the op-amp slew in the Distortion masks much of the germanium pair's softness (it's the quieter, more compressed one).
- Two older tests are timing-sensitive on a loaded machine: M5's ALSA port test (a 150 ms wait for the port) and M14's pixel-identical theme round trip. They pass on a quiet run.

## Milestone 19: themes, the compact Clip View, the overview bar, the IR library, the Drum Studio (done)

Asked for after 0.18: "another library option for Impulse Responses", "more themes for the UI, with better color combinations (and make it customizable with a customization window)", "a Drum Studio with similar capabilities as Superior Drummer: midi clips of many different styles of drum loops, several kits, microphone options", and the Clip View made compact ("very large and wide menus on the left side … small buttons and menus but still clear and legible … a device chain preview at the bottom and a clip preview").

### Themes (`core/Theme.h/.cpp`, `ui/ThemeEditor.*`, `MainWindow::rebuildThemeMenu`)

- A `theme::Theme` is a base palette (`Kind` Dark / Mid / Light), a tint (`hue` 0 .. 360 or -1, `tint` 0 .. 1, `contrast` -1 .. 1), an `accent` (invalid = the base's) and `overrides` (colour name → value). `resolve(theme, name)` derives one colour: the base's value; a *tintable* grey (saturation under 0.14, not a signal colour: `isTintable`) takes the hue at a saturation of `0.42 × tint` scaled down towards black and white, and has its lightness stretched from the background's by `contrast`; the accent family (`accent`, `devCurve`, `devCurveFill`, `readout` in the dark: `isAccentColour`) takes the accent with its own alpha; `onAccent` / `devButtonOnText` become light ink when the accent's luma is under 0.55. Overrides win. `apply(const Theme&)` fills the globals through `resolve`; `apply(Kind)` applies the three base themes, which change nothing (Dark stays pixel-identical; M14's test still checks).
- Twelve built-in themes (`builtinThemes()`): Dark, Mid, Light, then Midnight (blue-black, amber), Graphite (cool neutral, teal), Ember (warm brown, coral), Forest (green, lime), Plum (purple, pink), Slate (Mid, blue-grey, gold), Paper (Light, warm, burnt orange), Sky (Light, cool, blue with light ink), High Contrast (the dark stretched, yellow). Your own are `.sessionstheme` JSON files (`toJson` / `readTheme`) in `theme::folder()` (`~/.local/share/Sessions/Sessions/Themes`, `SESSIONS_THEMES` overrides it): `saveCustom`, `removeCustom`, `customThemes`, `themeNamed`. `notifier()->listChanged()` tells the menu and the editor. QSettings `ui/theme` keeps `dark` / `mid` / `light` for the bases (so 0.14's setting still reads) or the theme's name; `savedTheme()` / `savedKind()`.
- The editor (View > Theme > Customize…, `ThemeEditor`, non-modal): Start from (every theme), Name, Save (as yours; a built-in name gets "(mine)"), Delete, Revert; Base, Tint the greys + Hue, Amount, Contrast, the Accent swatch with ↺; every colour by group (`theme::group`: Window, Transport, Session View, Arrangement, Editors, Piano Roll, Widgets, Device View, Device Panels) as a swatch (QColorDialog with alpha) and a ↺ when overridden. Everything applies live through `theme::apply`. View > Theme is rebuilt from the lists: the three bases, the tinted ones, yours, Customize….
- Found on the way: the knob cells' label and value styles in `DeviceBody.cpp` were literal greys, pale on the light themes; they're built from `theme::devLabel` / `devValue` / `devCyan` now (the Device View already rebuilt its panels on a theme change), and two other `#9c9c9c` labels in the bodies went through `col::label`.

### The Clip View (`ui/ClipView.*`, the stylesheet)

- The boxes are 132 px wide (`kBoxWidth`; were 176), their title strips 16 px, margins 4 px, forms at 4 / 3 px spacing; the stylesheet gives everything inside `#clipProps` 12 .. 15 px tall fields with 10 px text (`QWidget#clipProps QPushButton …`). The tempo line is an `ElidedLabel` (its `text()` whole, drawn elided, the whole as a tooltip).
- round switches at the bottom left (`#clipToggles`, `QToolButton#clipBoxToggle`): **L** shows / hides the Launch box, **♪** the Notes or Sample box, **E** the Envelopes box; the Clip box always shows. Remembered in QSettings `clipView/launch`, `notes`, `envelopes` (all on by default). `showBoxes()` applies them and the Notes / Sample choice.

### The overview bar (`ui/OverviewBar.*`)

- A 26 px strip along the bottom of the window (under the main splitter, a status bar): the selection in words on the left ("Keys · Chords · 2 bars · 12 notes"), then `ClipOverview` (300 px: the selected clip's notes in the track's colour over its pitch range, or its waveform through its warp map; bar lines; the playhead when the clip plays on its track: `Project::playingClip`), then `ChainOverview` (440 px: one block per top-level device block of the selected track, `deviceBlockEnd`; instruments in the accent, MIDI effects in `devCyanDark`, effects in `deviceTitle`; off = `devicePanelOff`; the Device View's selection outlined, through `setSelectionProvider`; tooltips name them). Clicking the clip overview opens the Clip View; clicking a block opens the Device View with that device selected. It repaints on the project's signals and a 50 ms timer (which also refreshes the status text when the selection changed without a signal).

### Impulse responses (`core/Import.*`, `audio/Builtin.*`, `DeviceType::Cabinet`, `Browser`, `DeviceDrag`, `GuitarBodies.cpp`, `Mapping.cpp`)

- **Built-in IRs:** `audio::cabinetImpulse(cab, mic, sr, frames)` (in `Guitar.cpp`) renders one of the amp's five cabinets through a mic (Far adds the amp's two room taps) as an impulse, faded at its end; `builtinSample("builtin:ir/<cab>/<mic>")` wraps it as a sample (8192 frames at 48 kHz, cached), `builtinName` says "4x12 Closed · On Axis", `builtinIrPaths()` lists the fifteen. They save and load by name like the drum sounds (`writePath` / `readPath` already keep `builtin:` paths).
- **The library:** `import::irFolder()` (`~/.local/share/Sessions/Sessions/Impulse Responses`, `SESSIONS_IRS` overrides it) and your IR folders (QSettings `browser/irFolders`: `addIrFolder` / `removeIrFolder`); `irLibrary()` = the built-ins, then every audio file below them (`irsIn`, 5000 per folder), sorted; `irIndex`. `loadIr(p, track, device, path)` puts an IR (file or built-in) into a device that holds one (`holdsIr`: Guitar Amp, NAM, Cabinet) and switches it in (the amp's Cab → Loaded IR, NAM's IR on, the Cabinet's IR on); `stepIr` is `stepCapture`'s twin (wrapping; an IR outside the library steps through its own folder; none loaded starts at the first). `irDevice(p, track)` is the track's last IR holder. Mapping targets `device:<t>:<d>:irnext` / `:irprev` (buttons, "Next IR" / "Previous IR").
- **The Cabinet** (`cabinet::` IR, Mix, Gain, Low Cut, High Cut; in Audio Effects and Guitar): the Convolver at Mix against the dry sound, Low Cut / High Cut biquads (the lowest / highest position = off), Gain; no IR or IR off = dry. `Engine::publish` prepares the kernel for every `holdsIr` device. `CabinetBody`: the IR view (waveform, name, "13 / 15"), ◀ ▶ Load… Clear, the switch and knobs, the in / out meter; 430 px. The amp and NAM got ◀ ▶ and the "n / N" place beside their Load IR (`addIrSteps`, `IrPlace`), their `loadIr` goes through `import::loadIr`, and all three take browser IRs dropped on them (`Browser::irFilesIn`) as well as WAVs.
- **The browser:** Categories → **Impulse Responses** (◠): the built-ins, then the folder's and your IR folders' files, as `"path|r"` items (file URLs too, so the file manager sees them); right-click the category to show the folder or add an IR folder; IR folders are Places (◠ name, right-click to remove) with **＋ Add IR Folder…** after Add Folder…. The sidebar is 128 px wide now. Double-click an IR: `devicedrag::drop` with `Payload::Ir` on the selected track.
- **Drops:** `Payload::Ir` (`"path|r"`): `shape` is a Cabinet; `drop` on a track with an IR holder (no explicit place) loads it into the last one ("4x12 Vintage · On Axis loaded — Into the NAM on Guitar"), else a Cabinet holding it lands where it was dropped (a new audio track on the empty space).

### The Drum Studio (`DeviceType::DrumStudio`, `drumstudio::`, `audio/DrumStudio.cpp`, `core/Grooves.*`, `ui/devices/DrumStudioBody.cpp`)

- **Parameters** (50): Kit (six), Volume, Overheads (dB, -60 = off), Room (dB), Room Size, Bleed, Width, Velocity (the curve: 0 flat, 1 wide), Wires (the snare's), Beater (the kick's click); then Level / Pan / Tune / Decay (× 0.25 .. 4) / Tone per drum for Kick, Snare, Hi-Hat, Tom 1, Tom 2, Floor Tom, Ride, Crash (`param(drum, knob)`). `drumFor(pitch)` maps the GM notes (35 / 36 kick; 37 side stick, 38 snare, 39 clap, 40 rimshot; 42 / 44 / 46 hats; 41 / 43, 45 / 47, 48 / 50 toms; 49 / 57 crash, 55 splash, 52 china; 51 ride, 53 bell, 59 edge); `noteFor(drum)` auditions.
- **Synthesis** (`DrumStudio.cpp`): a `KitVoicing` table (Studio Rock, Vintage, Tight Pop, Metal, Jazz, Big Room) gives every drum its numbers; a `Voice` is up to three decaying sine modes with a shared pitch sweep (kick, toms, the snare's body), filtered noise with its own envelope (wires, the beater's slap, a cymbal's wash, the clap's bursts), a bank of eight square partials at inharmonic ratios through a high-pass (hats, ride, crash), a click, an optional shell low-pass and a soft saturation. Velocity goes through the curve to the level, the brightness and the sweep; a closed or pedal hat chokes the open one. 24 voices, the same drum retriggers its own voice (cymbals don't).
- **Mics** (`mixMics`): the close mics are the voices themselves, panned by Pan; the overheads every voice again, placed across the field by where the drum stands (`kOhPan` × Width), each side a hair late (1.2 / 2.1 ms × Width), brightened by the kit's `ohBright`; the room a mono feed through a pre-delay (6 .. 22 ms by Room Size) into four Freeverb combs and two all-passes a side (lengths × Room Size, feedback 0.6 .. 0.9), at Room; Bleed adds a 1.8 kHz low-passed copy of the overheads into the close mix. meter[0] / [1] light the panel's kit (the drum, a hit counter); meter[2] is the room's level in dB.
- **Grooves** (`grooves::`): 62 patterns in 14 styles (Rock, Pop, Funk, Hip Hop, House, Drum & Bass, Metal, Jazz, Reggae, Latin, Blues, Punk, R&B, Fills) written as sixteenth lanes (`K|x...x...`: K S H R C 1 2 3 P; `x X o f`, `O p` on H, `r R` on S, `b` on R, `c s` on C), a bar or two, with a swing (0.5 .. 0.67; `swing8` moves the off-eighths for shuffles instead of the off-sixteenths) and a written tempo. `notes()` makes clip notes (flams get a grace note 1/32 before), `humanVel` / `humanTime` add ±15 % and ±1/64 beat from a seeded hash (deterministic). `makeClip` names the clip "<style> <name>"; `toSession` / `toArrangement` put it on a MIDI track, or make a "Drums" MIDI track with a Drum Studio after an audio track (or the last). The drag payload is `kMime` ("index|vel|time"); `ClipGrid` and `ArrangementView` take it (a MIDI slot or lane; anywhere else a new track).
- **Panel** (`DrumStudioBody`, three pages on buttons like the Monolith's, `studioPage`): **Kit** — `KitView` (the kit from above; click a drum to select it and hear it through `Transport::previewNote`; a hit lights it), the Kit chooser, the selected drum's five knobs (one page per drum, `studioDrums()` remembers the choice), Wires, Beater, Volume. **Mics** — `MicView` (close mics on the drums, the overhead pair spread by Width, the room pair back by Room Size, the room's level) and the six mic knobs. **Grooves** — the styles, the grooves of one (`GrooveList` starts the drag), the groove's description, Humanize Vel / Time, **To Clip** (the track's next free slot, one undo step; double-click does the same).

### Tests (61 new; 828 in all)

- Themes: the twelve built-ins; Midnight tints the greys (hue, same lightness) and swaps the accent family, not the transport colours; Sky's light ink; High Contrast; a custom theme's save / list / read-back with overrides / apply / remember by name / delete; tintable and accent colours, groups; back to Dark identical; the editor (Start from, Hue live, a swatch override and ↺, Save as yours); the menu (built-ins, yours checked, Customize…, picking, a deleted theme leaving).
- Clip View: 132 px boxes, the three switches hiding and showing Launch and Envelopes (remembered). The overview bar: its strip, one block per device in order after a slot click, a block click opening the Device View with the device selected, the clip overview click, the notes painted in the track's colour.
- IRs: fifteen built-ins named, one rendered (a decaying thump, faded); the library from the folder (subfolders) and an IR folder, sorted; loading into the amp (Cab → Loaded IR), ▶ ◀ wrapping, an IR outside the library, the mapping targets' names; NAM and Cabinet switching on; the `|r` payload, dropped onto a track with a holder (into it, no new device), without one (a Cabinet), on the empty space (a new track); the Cabinet darkening noise, bypassing, Low Cut; ◀ ▶ on all three panels fitting; a built-in IR through the set file; the browser listing them.
- Drum Studio: all 22 GM drum notes sound (finite, under full scale; a note off the map is silent; the peaks are printed), kits differ, Decay / Level, Room's tail, Width's spread, the hat choke. Grooves: 62 in 14 styles all shaped, a shuffle's off-eighth on the triplet, Humanize deterministic and audible, the mime payload, a clip in a slot, a new Drum Studio track from an audio one, onto the timeline; the panel (pages, all 50 controls, displays, height, To Clip); a groove dropped on a slot and on a lane.
- Changed: M6's device count is 49; M18's pedal loop skips the Cabinet.

### Known gaps after M19

- Themes: a built-in theme's tint is applied to the three bases' greys only; coloured entries (track colours, the piano roll's in-scale shade) don't shift. The editor's swatch list is long (all ~150 colours); no search. A theme file edited by hand with unknown names drops them silently.
- Clip View: the Clip box's Signature and Start fields are still fixed (4/4, 1.1.1), as before; no colour chip or groove field.
- The overview bar's clip overview draws every note at its clip pitch range; a clip with one note fills the strip. The chain overview shows top-level blocks only (a rack is one block with its chain count).
- IRs: built-in IRs are the amp's cabinet filters as impulses (no measured speakers); the library scan is synchronous; no IR trimming / gain normalization beyond the Convolver's level match.
- Drum Studio: everything is synthesized (no sampled drums), so brushes, rolls and cymbal swells are approximations; no per-drum output routing or mic solo; the panel's kit view doesn't drag samples. Grooves are 4/4 only, no fills generator, no tempo-following swing.

## Milestone 20: the Monolith reworked (done)

Asked for after Milestone 19, with pictures of Massive and Massive X: "see how you can edit the envelope of the synth when playing a note … everything can be automated … the UI has bigger and smaller buttons, knobs and fields depending on what's more relevant … a basic UI for Monolith that can expand by clicking certain buttons, so an extra part slides out … big useful knobs for the oscs and main device knobs like gain, then specialised drawers that expand to the right with envelope editors, modulation, macros … make its sonic capabilities large, wide and flexible. Create presets for it as well."

### Parameters (`DeviceSpecs.h` `monolith::`, 120 → 138; `kMaxParams` 128 → 160)

- Appended after 0.17's (`kParams17` = 120; older sets get the defaults): **Sub Level / Sub Oct (-1, -2) / Sub Wave (Sine, Square, Triangle)** (120 .. 122), **F Drive** (123), **Mod Wave** (Sine, Triangle, Saw, Square; 124), **LFO 1 / 2 Mode** (Loop, One-shot; `lmode(l)`, 125 .. 126), then the FX from `kFxBase` = 127 (`fx(FxKnob)`): Chorus Rate / Depth / Mix, Delay Time (the Delay's synced choices) / Feedback / Tone / Mix, Reverb Size / Mix, EQ Low / High (±12 dB). `sectionName` knows the new sections (Sub, Mod Osc, LFO n, Chorus, Delay, Reverb, EQ). `kMaxParams` went to 160 (`DeviceSlot::params`, `Builtin.cpp`'s scratch array and the tests follow it; `deviceMergeKey` keeps 8 bits for the parameter).
- `DeviceProcessor::scope[16]` (atomics, like `meter`): what a panel animates from inside the processor; `Engine::deviceScope` and `DeviceBody::scope(k)` read it. The Monolith writes the newest voice's four envelope levels (0 .. 3) and stages (4 .. 7: `Adsr::Stage`, 0 idle .. 4 release), its LFOs (8, 9) and steppers (10, 11).

### Engine (`audio/Monolith.cpp`)

- The sub: one wave (sine, naive square or triangle, `shapeAt`) at the note an octave or two down, at Sub Level × 0.7, straight into filter 1's input. **F Drive**: `tanh(x · (1 + 5 · drive)) / √drive` on both filters' inputs. The modulation oscillator reads `shapeAt(ModOscWave)` instead of a sine. **One-shot** LFOs: treated as retriggered (their phase counts from the note) and clamped at the end of the first cycle, so they hold their last value: a saw-down LFO on the cutoff is a one-cycle envelope of any shape.
- The voices now sum into `m_fxL / m_fxR` and `render` runs the FX over the block once (`globalMods`, `processFx`), then adds them at the Volume. `globalMods` applies the routes whose targets are FX knobs with the *shared* sources only (macros, the shared LFOs and steppers, the wheel, pressure, bend; a voice's envelopes, velocity, key and random count nothing there), so a macro can bring the delay in. The chain: EQ (RBJ low shelf at 120 Hz, high shelf at 6 kHz, redesigned when the dB change) → the Dimension chorus (two 30 ms delay lines, each side's LFO a half cycle apart, 7 .. 13 ms by Depth, Mix blends) → the synced ping-pong delay (time = beats × 60 / tempo, 2.2 s of buffer a side, each side's feedback feeds the other through a one-pole Tone low-pass, the right input at 0.3 so the repeats bounce) → the reverb (four Freeverb combs and two all-passes a side, lengths 0.6 .. 1.8 × by Size, feedback 0.7 .. 0.92). Each stage is skipped when its Mix is 0 (the delay still pushes zeros so its buffer stays in time). `panic` clears them all.

### Panel (`InstrumentBodies.cpp` `MonolithBody`, `MonoEnvDisplay`, `MonoFxDisplay`; `DevicePanel::eventFilter`)

- **The main panel** (~1100 px, `buildMain`): three oscillator columns (table chooser, the cycle, Bend / Formant / Sync, then **Position** and **Level** big (30 px) with Pitch (24), and Intens / Fine / F1-F2 small (14)), a narrow Sub · Mod Osc · Noise / Feedback column (Sub Level, the octave and wave, the mod oscillator's three knobs, Noise / Color / Feedback), two filter columns (type, the response, **Cutoff** big, Reso, and Drive + Par/Ser on filter 1, Key + F Mix on filter 2 with the Insert under it), and Master (**Volume** at 36 px over the four macros).
- **The drawers** (`kDrawers` = 6, buttons `monoDrawer`: Env, LFO, Step, Mod, Voice, FX): a `QStackedWidget` in `m_drawerHost` whose `maximumWidth` a `QPropertyAnimation` (170 ms, OutCubic) slides between 0 and the page's width; `updateWidth` keeps the body's fixed width in step, and `DevicePanel` now watches its body's resize (an event filter) and follows, so the chain in the Device View makes room as the drawer opens. One drawer at a time (`setDrawer(k)`, `toggleDrawer`); `monoDrawers()` remembers the open one per device id for the session (-1 none). Dropping a source on a knob, or Modulate by, opens the Mod drawer.
  - **Env**: the four envelopes as `MonoEnvDisplay` (176 × 46, `AdsrDisplay` with the new `drawLive` hook and `point()` made protected): while a note plays, a dot rides the curve at the envelope's level (its x from the stage: along the attack by level, along the decay by how far it has fallen to the sustain, at the sustain's end, along the release by what's left), so you shape it as you hear it; A D S R knobs and the envelope's source chip under each.
  - **LFO**: the two LFOs (rate, the chip, the shape, **Loop / One-shot**, Free Hz, Fade, Retrig). **Step**: the two steppers (bigger, 170 × 70) with their chips. **Mod**: the 18 source chips, Add…, the routes list (226 × 158). **Voice**: Poly / Mono / Legato, Unison, the Mod Osc Wave (wave icons), Restart, Detune / Spread / Glide / Bend / Vel>Amp / Key>Cut at 22 px. **FX**: `MonoFxDisplay` (the four blocks EQ › Chorus › Delay › Reverb, lit when in use, with their values) over the EQ, Chorus, Delay (time chooser) and Reverb knobs.
- Found on the way: the knob cells' label styles were literal greys (fixed in M19); the device captures (`devices/13-monolith*.png`) now show the main panel and each drawer open.

### Sounds (nine more, `monolithPresets()`)

Dub Wobble (sub + drive + an LFO wobble, delay), Neuro Reese (two detuned saws and a sub into Scream and a band-pass, Fold, chorus, EQ), Talking Growl (Formant warping talked by two LFOs, legato), Supersaw Lead (three saws, unison 4, chorus, delay, reverb; Macro 1 darkens, Macro 2 brings the delay in, the wheel spreads the detune), Glass Pad (slow envelopes, two slow LFOs, chorus, a big reverb), Acid Monolith (LP 24 with resonance, a sharp filter envelope, a stepper on the cutoff, legato glide, delay), Hoover (Pulse synced, three saws, a one-shot pitch drop from Env 3 on all three oscillators, chorus), Stab (Metallic and Pulse, short envelopes, a long delay), Deep Sub (the Sub table with the sub oscillator, LP 12, mono, EQ Low +4 dB). All routed (`presetMods`).

### Tests (17 new; 845 in all)

- Engine: the sub adds weight an octave (and two) under the note, a square sub has harmonics; F Drive adds harmonics; a square modulation oscillator tears more than a sine; a one-shot saw-down LFO on the cutoff opens then closes once and holds (looping, it comes round); the delay repeats a stopped note half a second later at 120 BPM; the reverb's tail; the chorus parts the sides; EQ Low lifts the lows; Macro 1 → Delay Mix brings the delay in; the nine Sounds come with routes and play.
- Panel: the main panel ≤ 1120 px with six drawers closed and a control for all 138 knobs in the device's height; Env slides out and the `DevicePanel` grows with it; FX swaps the drawer; clicking the open drawer's button closes it; the open drawer is remembered per device (a rebuilt panel opens with it); the scope reports Env 1's level and stage while a note plays.
- Changed: M13's count is 138 (the engine passes up to 160); M13's panel test looks for six `monoDrawer` buttons, clicks Mod and waits for the slide (≤ 1600 px open); the M20 panel test closes whatever drawer the M13 test left open for that device id.

### Known gaps after M20

- The Monolith's main panel is wide (~1100 px) and the drawers add up to ~500 more: on a 1366 px laptop the Device View scrolls. A narrower "compact" main panel (one oscillator shown at a time) would be the next step.
- The drawer slides by width only; the Device View's chain doesn't animate the devices after it (they jump as the panel's width changes each frame).
- The envelope dot's position along the decay and release is derived from the level, not the time, so a very long decay with a high sustain moves it in a lump; the attack reads by level too (fine for the short attacks basses use).
- The FX are per device and shared by all its voices (as in Massive); the delay's time is synced only (no free ms), the reverb has no pre-delay or damping knob, the chorus no feedback (a flanger). Unison still caps at 4; no per-oscillator key-tracking switch.

## Milestone 21: after the 0.20 hands-on (done)

The user's list, with screenshots: "guitar pedals in audio effects, move everything guitar related under Guitar · the top bar (arrangement preview) is a scroll bar and a zoom bar: drag sideways scrolls, up or down zooms (a preference swaps the zoom direction) · the same for the clip/waveform preview at the bottom, and at the top of the clip preview between the two time marker areas · the device preview at the bottom should show mini versions of the actual devices, not labels, with the scroll, not zoom · select an area of a clip with the mouse and copy/cut that part as a new nondestructive clip (extend it and the rest of the sample is there) · Ctrl+click and drag a clip (or a selected part) to a new spot duplicates it · move the Clip and Device buttons between the info panel and the clip information, on their side · the record button should turn off when the track is stopped · warping only offers transients and re-pitch, there's no Beats/Complex/Complex Pro · a clearer distinction between beats on the grids (alternating lightness)".

### Browser (`Browser.cpp` `catalogue`)

- Everything for the guitar lives under **Guitar** only: `isGuitarType` devices (the Tuner, the Guitar Amp, NAM, the pedals, the Cabinet) left Audio Effects, the `RackRecipe::guitar` rigs left Sounds. The ordering inside Guitar is as before (Tuner, amp and NAM, pedals, rigs).

### The overview strips (`ArrangementView`, `OverviewBar`, `Preferences`)

- `PreferencesDialog::dragZoom(dy)` is the one zoom-by-drag rule: `exp(±dy · 0.012)`, the sign from **Look → Zoom drag** (`ui/zoomDragUp`, cached in a static; `zoomDragUp()` / `setZoomDragUp`). The Arrangement's ruler, the piano roll's ruler, the sample editor's ruler (new: `Drag::Ruler`, with `m_userZoomed` so a resize no longer refits a zoomed view) and both overview strips use it.
- The Arrangement's overview (`Drag::Overview`): the press remembers the song beat under the pointer, the zoom and the offset; a move zooms by the vertical drag around that beat (it stays where it was on screen) and scrolls by the sideways drag (the outline follows the pointer, `m_overK` = overview pixels per beat); a click without a drag still centres the view there (on release now, `centerOn`).
- `ClipOverview` (the bottom bar's, and a second one in MainWindow's detail header, `m_headerClip`, spanning the header after the title): `setViewport(get, set)` hands it the editor's viewport (`ClipView::editorViewport` / `setEditorViewport`, which go to `PianoRoll::viewport` / `setViewport` or `SampleEditor::viewport` / `setViewport`; false for the envelope and takes editors). It outlines the viewport in `theme::overviewView` and drags it: sideways scrolls (the outline follows), up / down zooms around the pressed beat (`dragZoom`); a click without a drag emits `clicked` (the bottom bar's opens the Clip View, the header's centres the editor on that beat, `beatAt`). The fixed size moved out of the class (the bar sets it).
- `ChainOverview`: `setThumbProvider` (MainWindow: `DeviceView::blockWidgets()`, the chain's top-level widgets with their device ids, `grab()`bed; a hidden Device View is resized to its size hint first, and `render` lays it out) and `refreshThumbs()` scale each panel's picture to the strip's height (`m_pictures`); `blocks()` lays the pictures out at their own widths, scrolled by `m_scroll` (drag sideways: the chain follows the pointer; the wheel too; a click without a drag selects as before); a device that's off is shaded, the selection outlined, a shadow at an edge says there's more. Without pictures (no Device View, the tests' bare bars) the labelled blocks remain. The bar's timer re-grabs twice a second while visible, and every `refresh()` does. `kBarHeight` 26 → 40, so the pictures are legible.

### Parts of clips, Ctrl+drag copies (`ArrangementView`, `MainWindow`)

- A press on a clip's **name bar** (its top 14 px) or its right edge moves / resizes it as before; a press on its **body** selects the clip (Play starts at it) and starts a time selection there, so a drag across the body selects that part (the selection is painted after the clips now, so it shows over them). `deleteSelection` and `duplicateSelection` act on a real time selection before the selected clip. The cursor says which: open hand on the name bar, I-beam on the body, open hand (copy cursor with Ctrl) inside a selected part.
- `copyRange()`: the material under the time selection as `ClipPiece`s (lane and start relative to the selection, the overlap's length, the placement's offset plus how far in the selection starts, and a copy of the **whole** clip): a part is a clip of its own that still holds everything, so extending the placement shows the rest. `MainWindow::copySelection` keeps them in `m_pieces` (and the first clip in `m_clipboard` for slots); `pasteSelection` in the Arrangement with pieces calls `pastePieces` (at the insert marker, lanes from the selected track down, each piece cleared in and placed, the result selected); `cutSelection` (new **Edit → Cut**, Ctrl+X) copies then `cutRange()` (`clearRange` on the selection) — a selected clip or slot clip is copied and deleted.
- `Drag::MoveRange` (`beginRangeDrag(copy)`): a press inside a selected part drags that part: without Ctrl the clips are split at the selection's ends (`arrangement::split`) and the pieces inside move together (`m_rangeDrag` as (lane, index), `m_rangeStart0`), the selection moving with them; with Ctrl, copies of the parts are appended and dragged instead, the clips staying whole. Ctrl on a name bar press appends a copy of the whole clip and drags that (`m_copyDrag`, `m_copyOrig`). A Ctrl press that doesn't move takes the copies back before the edit closes (no undo step). Moves snap as `Drag::Move` does.

### The tabs, the record button (`MainWindow`, `Transport`)

- `SideTab` (MainWindow.cpp): a `QToolButton` painting its stylesheet frame with no text and its text rotated 90°; the two sit in `#detailTabs` (20 px wide, `QVBoxLayout`) between the Info View and the detail pane. The header keeps the title label (no stretch) and the clip overview.
- `Transport::stop()` switches `m_arrRecord` off (and emits `recordingChanged`) after finishing the takes; `m_loopClips` is left alone, because audio takes still in flight land in them.

### Warp modes (`Model.h`, `audio/Stretch.*`, `Engine`, `ClipView`, `SetFile`)

- `WarpMode` is now the six classic modes in the chooser's order: Beats, Tones, Texture, Repitch, Complex, ComplexPro (`warpModeName`); sets save it by word (`kWarps`), so old sets' "beats" / "repitch" still read. New clip fields `grainMs` (12 .. 256, 50), `flux` (0 .. 1, 0.25), `formants` (0 .. 1, 0), in `operator==` and the set file. Transpose is enabled in every mode but Re-Pitch.
- `audio::Stretcher`: one placement's state (`Engine::m_stretch[12]`, keyed by clip id and placement origin in `m_stretchKey`, `stretchVoice()`; a voice not used in a block is free; it restarts itself when the clip beat it's fed jumps by more than ¾ of a sample). Fed a sample at a time from `renderAudio` (`StretchSource`: the sample, the warp map, tempo, ratio, settings), after the comp branch, with the engine's edge fades applied as for the other modes.
  - **Tones / Texture** (`granular`): grains of `grainSec` (Hann, spawned half a grain apart so the windows sum to one, at most 12 alive), each starting where the warp map says the output time is; Tones lines each grain up with the newest one's continuation by cross-correlation within ± a quarter grain (coarse every 8 frames, then fine); Texture jitters the start by ± Flux × grain and the length by ± Flux / 2, and the sum is normalised by the window sum (floor 0.5). Transposition reads the grains at `ratio`.
  - **Complex / Complex Pro** (`vocoder`, `frame`): a 2048-point frame every 512 output samples (both channels in one complex FFT, left real, right imaginary; the inverse the same way), Hann on both sides, the output overlap-added into a 4 × 2048 ring (the last hop of each frame's span zeroed before adding). Frame j's centre is at output time j·512 / (rate · ratio); its analysis centre is the warp map's seconds there; the phase advance per bin is estimated from the actual analysis hop (any size, the warp map sets it) and applied over the synthesis hop; the first frame after a restart copies the phases. Two frames of pre-roll keep the first samples at full overlap. The output is read at `t · rate · ratio + N/2` with cubic interpolation, so transposition is a resampling of the stretched stream (and the stretch absorbs it: the beats stay). Pro: identity phase locking (every bin follows the nearest peak, a bin topping its neighbours two either side, by `syn[k] = syn[p] + φ[k] − φ[p]`), a restart of the phases when the magnitudes grow by more than twice their sum between frames (a transient), and Formants: the cepstral envelope (liftered at rate / 800 quefrencies) of each channel, and bin k's magnitude scaled by `exp(formants · (env(k · ratio) − env(k)))`, so after the resampling the envelope sits where it was.
- Clip View: the Warp chooser (`warpMode`) lists the six; the row under it (`m_warpExtraLabel`) shows the mode's own controls: Slices (Beats), Grain (Tones), Grain + Flux (Texture), Formants (Complex Pro; shown disabled for Complex). The sample editor's caption names the mode.

### Beat shading (`ui/GridShade.h`, `theme::beatAlt`)

- `gridshade::paint`: every second beat (or bar when beats are under 7 px, or group of four bars) is filled with `theme::beatAlt` (a translucent white in Dark and Mid, a translucent black in Light) over the rows' background and under the grid lines, in the piano roll, the Arrangement's lanes, the sample editor, the envelope editor and the takes editor. (Dark is no longer pixel-identical to 0.20 here, by request.)

### Tests (30 new; 877 in all)

- The browser's Guitar category; Stop switches record off; `dragZoom` both ways; the Arrangement's overview drag scrolls, zooms, and zooms the other way with the preference; a clip body drag selects a part (the clip stays), `copyRange` / `pastePieces` / `cutRange` (one undo step), Ctrl+drag of a name bar duplicates, Ctrl+drag of a part copies it, a plain drag of a part splits it out and moves it; the side tabs' place and shape; the bottom bar's clip overview zooms and scrolls the piano roll, the header strip scrolls it back; the chain pictures at the panels' proportions (and a long chain scrolls); the six warp modes on a 220 Hz sine at half the tempo (Re-Pitch 110 Hz; the other four 220 Hz for twice as long, smooth: Tones / Complex ≥ 0.6 quiet-to-loud, Texture ≥ 0.25), transpose in Complex (+12 → 440 Hz) and Tones (−12 → 110 Hz), Complex Pro with Formants; the Clip View's chooser and per-mode rows; the set file round-trips the new fields; the piano roll shades every second beat.
- Changed: the M1 clip drag starts from the name bar; the M19 bar height reads `OverviewBar::kBarHeight`.

### Second pass (the same day, after the first hands-on)

"The waveform preview is too wide: it should go from the piano roll to the right · I can zoom into a waveform and go to beat -27000: zoom and scroll to 0.0.0 only, zoom into the mouse location · the clip/waveform editor should be really easy to use to move the waveform around and warp points (see how the classic DAWs do it) · the beat differentiators are cool but too much contrast, and make them dynamic for large clips · dragging MIDI devices onto an empty spot or an audio track should work (a new MIDI track / the track changes kind, and vice versa) · the play buttons look distorted; tighten all the icons."

- **The strip over the editor** moved from MainWindow's detail header into `ClipView` (`m_strip`, `overviewStrip()`): a column holds it over `m_editors`, so it is exactly as wide as the editor and starts where it starts. The header keeps the title only.
- **Views stay in bounds:** `SampleEditor::extent()` (from 1.1.1, or the audio before it, to the audio's end or the clip's) and `clampView()`: the zoom goes no further out than the whole sample and no further in than 512 px a beat, the offset stays inside the extent; `setViewport`, the ruler drag, the new `wheelEvent` (wheel scrolls, Ctrl+wheel zooms around the pointer) and the waveform drag all go through it. `PianoRoll::clampView()` the same (0 .. a beat past the clip's end; zooming out stops when the clip fits), with the zoom floor lowered from 6 to 1 px a beat so long clips fit.
- **The sample editor, after the classic sample display:** a 12 px marker strip under the ruler (`kStripH`, `kTop`) holds the markers' handles (pentagons) and the sample's transients as ticks (`transientAt`); hovering lights a tick, dragging it inserts a marker at that moment (`insertMarker`) and warps it, double-clicking it makes a marker exactly there. A plain drag on the waveform scrolls (`Drag::Scroll`), Shift+drag slides the audio under the grid (`Drag::Slide`: every marker's beat moves by the snapped delta, one undo step "Slide Audio"). The right-click menu has `setStartHere` (a marker is pinned at the beat if none is there, then every marker's beat shifts so that moment is 1.1.1), `warpFromHere(beat, bpm, startHere)` (the markers before the beat stay — or go, with startHere — the beat is pinned to its audio and one end marker runs the rest of the file straight at `bpm`: the found tempo for Warp From Here, the clip's own for Straight, the set's for Warp N BPM From Here), Add / Delete Warp Marker and Reset Warp Markers (`Project::syncClipToTempo`). The snap grid gains 1/16 above 200 px a beat; the ruler labels thin out at low zooms.
- **Beat shading:** `theme::beatAlt` is about half as strong (alpha 11 / 13 / 10), and `gridshade::paint` picks the smallest unit of 1, 4, 16 or 64 beats whose band is at least 28 px, so a long clip alternates per bar or per four bars.
- **Tracks change kind:** `Project::canConvertTrack` / `convertTrack(index, to)`: a MIDI or audio track (no returns, no groups) with no clips, and going to audio no instrument or MIDI effects, switches `type` (arm off, monitor Auto, input 1/2), one undo step. `devicedrag::accepts` says yes to an instrument or MIDI effect on such an audio track (`wouldConvert`), and `drop` converts it inside the drop's undo step before placing ("On X, now a MIDI track."). `import::audioTrackFor` converts an empty MIDI track an audio file lands on (Session column drops, Arrangement lane drops) instead of making a track after it; the browser's double-click on a MIDI track still loads a Sampler. The Session View's Main column now counts as free space (a new track) for device drops.
- **Glyphs:** `ui/Icons.h`: `icons::paint(p, Glyph::Play / Stop / Record, box, colour)` draws antialiased shapes (a triangle whose centroid sits a little right of centre, a rounded square, a disc); `icons::icon(glyph, off, on)` makes a QIcon whose On state (a checked tool button) is the dark glyph the accent background needs. Used by the control bar's play / stop / record (`ControlBar::refreshIcons`, re-made on a theme change), the mixer strips' and the Arrangement headers' arm buttons, the browser's Preview, and every slot, group slot and scene triangle and record disc in the Session View (the font glyphs ▶ ■ ● are gone from the widgets).
- **Tests (+20):** the sample editor's clamping (beat -27000 stays home, no zooming out past the clip, Ctrl+wheel keeps the pointed beat), Shift+drag slides (and is one undo step), a plain drag scrolls, a transient tick becomes a marker, Set 1.1.1 Here, Warp From Here and its Start at 1.1.1 form, each undone in one step; the shading's unit at a low zoom and its faintness; a MIDI effect on an empty audio track converts it, the Main column makes a new track, an audio file on an empty MIDI track converts it (undo restores), a track with clips refuses; the transport buttons carry icons with an On state and the play glyph is a centred filled triangle. Changed: the M11 instrument-on-audio-track checks (Device View, Session) now expect the conversion, and an audio track with a clip still refuses; the M21 header-strip check looks for the strip over the editor.

### Third pass (the same day): live looping, the Master, colours, the waveform, the Monolith after Massive

"Double check that the Session View works flawlessly for live looping: when I press the play button on the right to launch the whole scene, some clips flash and don't play, even from 0.0.0 · the Main track: call it Master and verify all audio is routed through it (adding effects to it doesn't affect the sound) · more colour options for clips and tracks, more saturated ones, shown as coloured squares in a grid · the waveform in the clip editor shouldn't be the clip colour but a fixed one that contrasts with the grid · click somewhere in the waveform to start playing from there · Qwen's list of what Massive has that Monolith might be missing."

- **Scenes** (`Transport::launchScene`, `launch(track, scene, fromScene, immediate)`): the first launch of a scene from a standstill started the song, so every later track in the row was launched *while playing* with the launch quantization, and the engine queued it for the next bar: the flashing clips. `launchScene` now notes whether the song was stopped and launches every clip with `immediate` (quantization 0, `queuedAt` = now). While the song plays nothing changes: every clip lands on the same point.
- **The Master track** (`Project::master`, a `Track` of type Audio with `kMasterId`, name "Master"; `trackAt(trackCount())` returns it, `isMaster(i)`): the mix never had a chain of its own (a drop on the Main column made a new track, which is what the user heard: nothing). Now `Engine::publish` fills `RenderState::master` like a return (`fill`), and `renderBlock` runs `runChain` on `m_mainL / m_mainR` after the returns are mixed in and before the master fader and the soft clip. `EditState::master` carries its devices and automation through undo; the set file writes `"master"` (0.21; older sets have none) and its devices get ids with everyone else's. Selecting: the Session View's Master header (`Hit::MainHeader`) and the Arrangement's Master lane name select index `trackCount()` (`MainWindow::selectTrack` allows it, the Device View shows the chain, "Drop Audio Effects Here"); a double-click on the lane opens the Device View. Drops: the Session's Master column and the Arrangement's Master lane go to its chain when `devicedrag::accepts` (audio effects); an instrument there still makes a new track. `canConvertTrack` refuses the Master. Labels: "Main" → "Master" everywhere the user sees it (the Session header, the Arrangement lane, the mixer strip's Info texts, the undo label).
- **Colours** (`Project::palette()`, `kPaletteColumns` = 14): the palette is a grid built from fourteen hues in five rows (vivid S .86 L .62, pastel .88 / .75, soft .45 / .66, deep .75 / .42, muted .30 / .46) plus six greys: 76 colours. `addColorGrid(menu, current, pick)` (InlineEdit) puts a `ColorGrid` widget action into a menu: 18 px squares, the current one outlined, a click picks and closes the menus. The track menu's Color submenu uses it. `Clip::color` (0.21; invalid = the track's; `Project::clipColor(clip)`), saved as `"color"`, in `operator==`; the slot menu and the Arrangement clip menu offer the grid and **Track Color**; the slots, `drawClip`, the Clip box's name field, the piano roll, the sample / envelope / takes editors and the clip overviews paint with `clipColor`.
- **The waveform** (`theme::waveform`: a light blue-grey in Dark, a dark one in Mid and Light): the sample editor's waveform uses it (0.92 alpha, 0.3 past the clip's end); the ruler's loop bar and the length handle keep the clip's colour. The Arrangement's clips still draw their waveforms in `clipInk` on the clip colour.
- **Click to play** (`SampleEditor` `Drag::Scroll` without a move → `Transport::playClipFrom(clip, beat)`): a clip on the timeline (and not overridden by the Session) gets the start marker (and, while playing, the playhead) at the placement's beat for that clip beat; a Session clip is launched at once if it isn't playing, and `Command::SeekSlot` sets the clip's position: applied on the spot when the slot plays, or kept in `PlayState::seekBeat / hasSeek` until the queued launch lands (`slotStart = start - seekBeat`).
- **The Monolith after Massive** (what the list named that it lacked: more LFOs, more macros, shaped and looping envelopes; the rest it had): parameters appended after 0.20's (`kParams20` = 138 → `kParams` = 162; `kMaxParams` 160 → 192): LFO 3 and 4 as one block each (`Lfo2Knob`: Rate, Hz, Shape, Retrig, Fade, Mode; `kLfo2Base`), Macros 5 .. 8 (`kMacro2Base`), Env Curve ×4 (`kEnvCurveBase`, -1 .. 1) and Env Loop ×4 (`kEnvLoopBase`). `lparam / lmode / lretrig / lfade / macro` map the old and new blocks (`kLfosAll` = 4, `kMacrosAll` = 8), so the engine and the panel loop to four and eight. Sources appended: `SrcLfo3, SrcLfo4, SrcMacro5 .. 8` (`lfoOfSource`, `macroOfSource`; `kSources` 18 → 24). Engine: `lfoVal / m_lfo` sized `kLfosAll`, `source()` / `globalMods()` know the new ones, `scope[12..13]` carry LFO 3 / 4; `curved(level, c)` bends an envelope's level (`pow(x, 1 + 3c)` for c > 0, `1 - pow(1 - x, 1 - 3c)` for c < 0, skipped near 0); Loop puts an envelope back into its attack when it reaches the sustain (or the decay's end) while the note holds. Panel: four LFO columns in the LFO drawer, eight macros in two rows of four (18 px) under the Volume, a Curve knob and a Loop switch in every envelope's row.
- **Tests (+18):** a scene from a standstill starts every clip at once (and together on one point while playing); the Master (index, kinds, no conversion), its Utility at -35 dB quietens the whole mix, undo, save / load, drops on its column (effect: its chain; instrument: a new track), clicking its header selects it; the palette's size and vividness, a clip's own colour (undo, save / load, painted in the slot), the colour menu is one grid; the waveform's fixed colour, a click puts the start marker there / launches a Session clip there; the Monolith's new indices and sources, LFO 3 swings a level, Macro 5 ducks the Volume, Curve bends the attack, Loop keeps pulsing. Changed: M13's count is 162 (192 passed), M20's closed width bound is 1200 px (the macro rows).

### Fourth pass: the Monolith's drawn curves, clips from the left, no overlaps, the strip

"Monolith still doesn't feel right; I don't even understand how to automate things on it. It needs a curve editor to automate knobs: assign an envelope to a knob, simple or quite random, fully editable; ADSR isn't what I need for bass phrases · I can't extend recorded clips back to the beginning (the information is in the clips) · I can overlap clips on the arrangement, that's a no go: what gets placed is what's played · the waveform strip stayed green (and showed the previous clip) until I zoomed."

- **Curves** (`Device::curves`: `kCurves` = 2 lists of `CurvePoint {x, y}` in 0 .. 1, up to `kCurvePoints` = 32, sorted by x; saved as `"curves": [[[x, y], …], …]`; in `operator==(Device)`): parameters appended per curve (`cparam(c, CRate / CMode / CRetrig / CSmooth)`, `kCurveBase` = 162, `kParams` = 170), sources `SrcCurve1 / 2` (`kSources` = 26, unipolar). The engine copies the points into `DeviceSlot::curves` at publish and hands them to the processor through `Context::curves / curveCount`; `Monolith::curveAt` interpolates linearly (flat outside the points, 0 without any), `curvePhase` runs the length (`Rate` in beats, "Free" counts as a bar) from the song's start (or the note's, with Retrig), looping or clamped (One-shot), `curveSmooth` a one-pole (τ = 0.25 · Smooth² s) over the kSub steps; `scope[14 + c]` carries the phase for the display's line. Panel: a seventh drawer, **Curve** (`kDrawers` = 7, appended so the other drawers' indices stay), with `MonoCurveDisplay` per curve (click adds a point on the 1/16 grid, Alt free; drag moves one between its neighbours; double-click deletes; right-click: Ramp Up / Down, Triangle, Four Steps, Random 8 / 16 Steps, Random 8 Points, Clear) over Rate, Loop / One-shot, Retrig, Smooth and a **Random** button. Edits go through `MonolithBody::editCurve` (an `EditScope`, the device looked up after the snapshot, the list clamped and sorted); a drag is one step (`beginEdit` / `endEdit` around its moves).
- **Clips from the left** (`Drag::ResizeLeft`, within 6 px of a clip's left edge): the start moves on the grid, the length follows and the offset moves the other way so the audio stays put (`m_clipOffset0`); a looping clip wraps the offset, a one-shot can't start before its audio (the delta stops at `-offset`).
- **No overlaps** (`settleOverlaps`): on the release of a move or resize that moved, the clip is taken out, `arrangement::clearRange` cuts what lay under its span, and it goes back (the lane re-sorted, `m_sel` re-found); a range drag does the same for every piece. Everything else that places clips already cleared (paste, drops, recording).
- **The strip**: `ClipOverview` repaints on `selectionChanged`, `clipChanged`, `structureChanged`, `restored` and the transport's tick while playing, and `ClipView::setClip` / `setEditorViewport` poke it; it used to wait for whatever repainted it next.
- **Tests (+17):** the curves' parameters and sources; a gate curve silences half a bar and sounds the other, round and round; One-shot holds the end; Retrig starts with the note; the seventh drawer, two displays; a click adds a point on the grid, a drag moves it, Random makes sixteen points in one step, undo, double-click deletes, save / load keeps the curves; the left edge extends a looping clip back (offset wraps) and a one-shot only to its audio; a clip moved over another cuts it, a clip lengthened over the next cuts the next's start; the strip repaints on a new selection. Changed: M13 / M20 count 170 knobs and seven drawers.

### Known gaps after M21

- The overview strips zoom around the pressed beat; a drag in the overview can't resize the outline's ends.
- The sample editor's Warp From Here runs a straight tempo from the beat (the whole file's found tempo); the remainder's tempo isn't re-detected. There's no Shift+drag of a single marker to move its audio instead, and no pseudo-marker dragging beyond the transients. A track changes kind only while it's empty of clips.
- The chain pictures are re-grabbed twice a second (a `grab()` of every panel: a wide Monolith costs a few ms); a very long chain is scrolled only by dragging or the wheel, there's no scroll bar.
- Tones is WSOLA with a fixed search (a quarter grain, coarse-then-fine); polyphonic material still phases a little. The vocoder has no transient detection in plain Complex, and Pro's is a simple spectral-flux rule; Formants uses a cepstral envelope with a fixed lifter. Neither mode has an **Envelope** control. Stretchers are 12 per engine: more playing stretched placements than that fall back to Re-Pitch for the block.
- A part moved out of a clip is split out; pasting pieces across tracks of another kind skips those lanes silently.
- The Session View's slot clips can't be selected in parts (the request was about the Arrangement).
- The Master's device automation isn't shown in the Arrangement (no lane for it) and the master fader has no automation. The Master has no sends and no pan.
- A clip's colour doesn't follow into the Arrangement's overview strip (it paints track colours), and the Drum Studio's grooves make clips without one.
- The Monolith still lacks Massive's per-route modulation modes (multiply / scale) and LFO 3 / 4 in the panel's scope dot; its main panel is ~1150 px now. The curves are linear between points (no per-segment curvature yet), and only two of them; a curve's points don't snap to each other.

## Milestone 22: after the 0.21 hands-on (done)

The user's list, with screenshots of a sample display and of a recording: "the markers above the waveform: warp markers (yellow), the two triangles where the clip starts and ends, the loop region; they can all be dragged · recording Session clips onto the timeline shows an empty red region that updates now and then; when it stops the clips are transparent, and after Back to Arrangement they stay transparent until I click · show the recording solid, with a live waveform that updates smoothly · delete parts of a waveform by selecting them and pressing Delete, in the Arrangement and in the clip editor · save a clip to the library by right-clicking it, or from the clip editor · iterate on the right-click menus in every screen and section · make the browser a tree".

### Recording as it happens (`Transport`, `ArrangementView::drawTake`)

- `Transport::LivePeaks`: every audio take (timeline or Session slot) keeps the input's min / max per 256 frames from its first frame (`AudioTake::peaks`, filled in `drainCapture`'s `feed`, silence included). `livePeaks(trackId)` and `livePeaksFor(clip)` hand them to the views. `recordFolder()` is the folder recordings (and now edited samples) go to.
- The Arrangement draws a take in progress as the clip it's becoming, solid, with a red frame: a launched Session clip as that clip (name bar, waveform or notes, looping from its launch: a throwaway `ArrangementClip` through `drawClip` at alpha 1), the audio input in the track's colour with the live waveform, keys with the notes so far. It repaints every tick while something records (`audioRecording()` too).
- **Back to Arrangement left the clips dim:** the tick handler only repainted while a track was overridden, so the tick that cleared it never painted. It now repaints when the overridden state changes and on `Transport::sessionChanged`.
- The sample editor shows a Session slot being recorded (its sample isn't there yet) from the live peaks, with "● Recording".

### The sample editor (`SampleEditor`, `core/SampleEdit.*`)

- Rows from the top: ruler (16) · **loop brace** (8: the clip's colour from 1.1.1 to the length, with handles) · **start ▶ / end ◀ markers** (10) · the warp strip (12) · the waveform. `kBraceY`, `kMarkY`, `kStripY`, `kTop` = 46.
- Dragging the start marker along the audio shifts the whole warp map by the opposite amount and shortens the clip by it, and moves the view with it, so the audio stays put on screen and the grid moves: the clip starts later or earlier on its audio, the end stays where it was (`slideBy(beats, keepEnd)`; "Move Clip Start"). The brace's body slides the loop along the audio, length kept ("Move Loop"); its right end and the end marker set the length. This keeps the model as it was (a clip plays from 1.1.1 for `length` beats): the start marker is the Set 1.1.1 Here command made draggable.
- **Selection:** a plain drag across the waveform selects a stretch (snapped; Alt free; Shift+click extends; Esc clears; Ctrl+A all); a click still plays from there; the middle button scrolls now (a plain drag used to). `selection()`, `select()`, `hasSelection()`.
- **Delete** cuts the stretch out of the audio, **Crop to Selection** keeps only it, **Loop Selection** (Ctrl+L) makes it the clip. Samples are immutable and shared, so `sampleedit::cut` / `crop` write a new 32-bit float WAV (`"<name> (edit).wav"` in the record folder) and point the clip at it: the warp map, the length and the envelopes follow (after a cut, what came after plays earlier; a crop starts at 1.1.1), `reverse` is baked in, the original file is untouched (undo returns to it). Clips with takes refuse (their audio is spread over the takes).
- `MainWindow::Area::Sample`: Delete, Ctrl+A, Ctrl+L and copy route to the sample editor when it has focus (before, Delete there deleted the Session selection).
- The right-click menu: the selection commands, Add / Delete Warp Marker, Set 1.1.1 Here, **Set End Here**, the Warp From Here family, Reset Warp Markers, Reverse, Loop On / Off, Save Clip to User Library…, Show Sample in File Manager.

### The clip library (`core/Presets.*` clip functions, `SetFile::clipToJson`, `Browser`, `MainWindow`)

- `.sessionsclip` files in `presets::clipFolder()` (`Presets/Clips`): `{"format": "sessions-clip", "version": 1, "name", "clip": setfile::clipToJson}`. `saveClip` copies an audio clip's sample (and its takes) into `Clips/Samples` unless they're already under the library, and writes paths relative to the clip file too; `readClip` loads it (the sample with it); `renameClip`, `removeClip`, `listClips`, `clipNameOf`. `Library::changed` fires as for presets.
- Saving: **Save Clip to User Library…** in every clip menu (slot, timeline, piano roll, sample editor), the Clip box's **Save to Library** button and Edit → Save Clip to User Library… (`MainWindow::saveClipToLibrary`: asks for a name, offers to replace). The views ask the window through a `commandRequested(QString)` signal ("cut", "copy", "paste", "saveClip", "showSample"), so a context menu can run the window-wide commands.
- Loading: the browser lists clips under User Library → **Clips** (kind `c`, payload the file's path, with a file URL); double-click → `loadLibraryClip` (the Session grid's selected slot, or the Arrangement at the insert marker); drag onto a slot (`ClipGrid::placeClipFiles`: the next free slots down from there, scenes added) or a lane (`ArrangementView::placeClipFiles`: from that beat, over what was there); a track of another kind, or no track, gets a new track of the clip's kind. `Browser::clipFilesIn` reads them from a drag (browser items or `.sessionsclip` files from the file manager).

### Right-click menus (`ArrangementView`, `ClipGrid`, `PianoRoll`, `EnvelopeEditor`, `InlineEdit::showTrackMenu`, `MixerStrip`, `Browser`)

- Timeline clip: Cut / Copy / Paste, Duplicate, Delete, Rename (in place on the name bar, `renameSelectedClip`), Color, Loop On / Off, Reverse and Warp On / Off (audio), Loop Clip, Save Clip to User Library…, Show Sample in File Manager, then the time commands; with a part of the clip selected the first ones say "Selection". An empty lane: Paste (the insert marker goes where you clicked), Insert MIDI Clip (MIDI lanes; the selection's length when there is one), the selection's Cut / Copy / Delete.
- Session slot: Launch / Stop Clip, Cut / Copy / Paste, Duplicate, Delete, Rename, Color, Loop, Reverse / Warp, Save Clip…, Show Sample…; an empty slot: Insert MIDI Clip / Paste / Record into Slot (armed tracks); a stop button: Stop Track, Stop All Clips; the Master header: Stop All Clips, Back to Arrangement.
- Tracks (every header and now the mixer strips too): Track On / Off, Solo, Arm, Monitor (audio), Fold (groups) joined Rename, Color, Duplicate, Move, Delete, Group and Insert.
- Piano roll (new): Add Note Here, Cut / Copy / Paste (the clip), Duplicate, Delete, Select All, Select All Notes of This Key (on the keyboard), Octave / Semitone Up / Down, Nudge, the Grid submenu, Fold, Save Clip…
- Envelope editor (new): Delete Breakpoint / Add Breakpoint Here, Clear Envelope.
- Browser items: Load onto Selected Track, Preview (samples), Show in File Manager, Rename… / Delete (presets and clips); folder nodes: Expand / Collapse, Show in File Manager, Rescan.

### The browser as a tree (`Browser.cpp` `ItemTree`, `addFiles`, `node`, `leaf`)

- The content pane is a `QTreeWidget` (`browserItems`): a sample folder's subfolders are nodes (folders first, then files, sorted), closed until you open them, and what you opened stays open across rebuilds (`m_expanded`, by key). The User Library shows presets, a **Clips** node and a **Captures** node; Impulse Responses the built-in cabinets and your IRs under two nodes. A search flattens the matches under one node per source (category, folder, User Library). `listedItems()` walks the leaves, so the tests and `import::captureLibrary` see the same files. The drag payloads are unchanged ("name|kind" in `Qt::UserRole`, `kKeyRole` / `kPathRole` on nodes).

### Tests (20 new; 958 in all)

- M22: live peaks; the sample editor's selection by drag, Delete cuts the audio into a new file in `Project::recordFolder` and the map follows, undo brings the original back, Crop, the start marker drag (map shifted, clip shortened, the audio still under the pointer), the loop brace drag, Loop Selection; the clip library round trip for an audio clip (sample copied into the library) and a MIDI clip, the browser listing them, the drag payload, placing on a MIDI lane (a new audio track) and a slot (a copy), rename / delete; the browser tree (nodes, every file still listed, open folders stay open, a search flattens). The M21b "a plain drag scrolls" check became "a plain drag selects; the middle button scrolls", and the strip's test y uses `kStripY`.
- An offscreen harness (not kept) rendered the editor, the recording takes and the browser to PNGs for a look.

### Known gaps after M22

- ~~The clip's start marker is Set 1.1.1 Here made draggable~~ (M23: a real start marker and loop brace).
- Delete / Crop write a new file per edit (float, the sample's rate) into the record folder; nothing collects the edits a set no longer refers to (Collect All and Save gathers the ones it does).
- Live peaks come from the capture stream, so they trail the input by the output + input latency the file is shifted by (a few ms at 10 ms buffers).
- The piano roll's Cut / Copy / Paste act on the whole clip (there's no note clipboard yet); the Arrangement's clip rename edits in place but a clip narrower than 80 px gets a wider box.
- The browser's categories (devices) are still flat lists; Sounds aren't nested by instrument.

## Milestone 23: a real start marker and loop brace (done)

The classic model keeps a clip's start marker apart from its loop brace: a clip can start before the loop and run into it, start inside it, and with Loop off it plays from the start marker to the end marker once. Until 0.22 Sessions played every clip from 1.1.1 for `length` beats, and the sample editor's ▶ was Set 1.1.1 Here made draggable (it shifted the warp map). Now the model has both.

### The model (`Model.h` `Clip::start` / `loopStart` / `loopEnd()` / `positionAt` / `positionFrom`, `clipPositionAt`, `forClipPasses`, `ArrangementClip::beatOf`, `Project::clipPosition`)

- A clip's own timeline runs from 1.1.1 (beat 0). `start` is where playback begins; the loop brace is `[loopStart, loopStart + length)` and `loopStart + length` is the end marker. Both default to 0, so every existing set, the M4 round trip and every test that builds clips mean what they did.
- Playback runs in **passes**: pass 0 from `start` (a launched clip) or a placement's `offset` (the timeline) to the end marker; with Loop on every later pass goes round the brace; with Loop off there is only pass 0. `clipPositionAt(start, loopStart, length, loop, t)` gives the clip beat `t` beats after the launch (a position before 1.1.1, or past the end marker with Loop off, means silence: `Clip::positionAt` / `positionFrom` return −1 there); `forClipPasses(…, lo, hi, f)` calls `f(at, from, to)` for every pass overlapping `[lo, hi)` beats after the launch. Everything that used to `fmod` a position by the length goes through these: the engine, the ops, the views.
- `offset` keeps its meaning (the clip beat a placement starts at); it's the placement's own start marker, so a split's right half keeps `offset + delta` unwrapped and play time takes it round the brace. `Clip::start` is for launched (Session) clips; a Session clip recorded onto the timeline gets `offset = positionAt(take.start − slotStart)`.
- `ArrangementClip::beatOf(clipBeat)` finds the first timeline beat where a placement plays that clip beat (−1: never), for `Transport::playClipFrom`. `Project::playingClip` / `clipPosition` give the clip beat under the playhead for the views.
- Everything that scales a clip's grid (`rescaleClip`, `syncClipToTempo`) scales `start` and `loopStart` too; `sampleedit::cut` moves them with the audio (what was inside the cut lands on its start), `crop` and Loop Selection reset them to 1.1.1.

### The engine (`Engine.cpp` `ClipData::start` / `loopStart` / `end()` / `firstPass()`, `emitClip`, `addSegment` / `AudioSeg::start`, `renderAudio`, `clipOverrides`, `scheduleTrack`)

- `emitClip` and the audio render loop take the first pass's start (the clip's `start` for a slot, the `offset` for a placement) and run `forClipPasses` / `clipPositionAt`. A one-shot ends `end() − start` beats after the launch (`emitSource`'s `hi`, and the slot stops then). The loop-point declick fades out before the end marker and fades in after `loopStart` only once a pass has wrapped, never while running into the brace.
- Follow actions' "one loop" is `firstPass()` (launch → end marker). Legato takes over the old clip's **clip beat** and starts the new one so it's at that beat (`switchAt − (pos − newClip->start)`): before its start marker is fine, the position is just `start + elapsed`.
- Seeks (`Transport::playClipFrom`, `Command::SeekSlot`) carry beats-after-launch (`clipBeat − clip->start`), so clicking before the start marker plays from there; the engine no longer rejects a negative elapsed as "not started", only a position before 1.1.1.

### The views

- **Sample editor:** ▶ at `start` (its own row), the brace from `loopStart` for the length, ◀ at the end marker. Dragging ▶ moves `Clip::start` (snapped, Alt free; "Move Clip Start"), the brace's body moves `loopStart` ("Move Loop"), its left end moves `loopStart` with the end kept ("Move Loop Start"), its right end and ◀ set the length ("Clip Length"). The warp map stays put: **Set 1.1.1 Here** is still the command that moves the grid on the audio, unchanged. **Set End Here** measures from the loop's start. What plays (from the first of ▶ / the brace to ◀) is bright, the rest dimmed; `extent()` reaches past the markers. `slideBy` is gone.
- **Piano roll:** the ruler holds the same brace (its top 5 px: ends within 5 px, body between) and ▶ / ◀ (the 8 px under, within 6 px); `rulerHit` decides, the rest of the ruler still zooms and scrolls. The grid is shaded before the first marker and past the end marker; lines at ▶ and ◀; `clipExtent()` (fit, clamp). `kRulerH` for the tests.
- **Clip box:** **Start** is live (`Clip::start`, bars. beats. sixteenths from 1.1.1), **Position** is new (`loopStart`), Length as before.
- **Arrangement:** `drawClip` draws the waveform / notes and the loop seams per pass (`forClipPasses` from the offset); a left-edge resize wraps the offset into the brace only when it leaves the clip; a take in progress shows at `positionAt`. `arrangement::notesIn` (consolidate, Ctrl+C of a part) walks the same passes.
- **Overview strip:** spans 1.1.1 to the end marker (or past ▶), draws the brace along the top and a line at ▶. The clip clocks' `loopFraction` counts the run-in as a fraction of the first pass, then the brace.
- `SampleEditor`, `EnvelopeEditor`, `TakesEditor`, `PianoRoll` and `OverviewBar` read the playhead through `Project::clipPosition`.

### The set file (`SetFile.cpp` `writeClip` / `readClip`, `clipToJson`)

- `"start"` and `"loopStart"` per clip (clamped ≥ 0 on read; missing → 0, so 0.22 files play as they did), in the clip library's JSON too; compared in `operator==(const Clip&)`, so the M4 round trip and `EditState` see them.

### Tests (32 new; 990 in all)

- M23: the engine renders a four-level audio clip launched with start = 2 (its third beat first, then round the brace from 1.1.1), a brace away from 1.1.1 run into and looped, Loop off from ▶ to ◀ once and the slot stopping; a MIDI launch with start = 0.5; a placement running from its offset into the brace and looping it (and `notesIn` / `beatOf` seeing the same passes); `positionAt` / `positionFrom` / `playingClip` / `clipPosition`; save / load of both fields (the set, the clip library's JSON, a 0.22 clip without them, `EditState` ≠); the sample editor's ▶ (the map and the view stay), the brace's body, its left end, ◀, with undo; the piano roll's ruler (▶, the body, both ends, the rest still zooms); the Clip box's Start / Position; `playClipFrom` through the passes and through the start marker; Set End Here from the loop's start; Loop Selection resetting the markers. The M4 round trip carries `start = 1.5`, `loopStart = 0.75`.
- The M22 "dragging the start marker shifts the map" and "the brace slides the loop along the audio" checks became their M23 versions (the fields move, the map stays).

### Known gaps after M23

- Set 1.1.1 Here shifts the warp map under markers that stay on the grid (so ▶ and the brace play different audio after it), as the envelopes do; moving them with the audio would be the alternative. Loop Selection still shifts the map so the selection is 1.1.1 rather than just setting ▶ and the brace to it.
- Takes / comps are in clip beats from 1.1.1 for `length` beats: a loop-recorded clip's brace is meant to stay at 1.1.1 (moving it plays the newest take past the comp). Recording into a Session clip lands notes at beats after the launch, not through its start marker (recording clips are new, so it's the same thing today).
- The envelope editor draws the envelope over 1.1.1 .. length only; it doesn't show the brace or ▶ yet.

## Milestone 24: the logo, the About, launching after a stop (done)

The user brought the Sessions logo (a six-segment S built from clip-shaped blocks, and the wordmark) and asked for it as the app's identity, a coloured version for the app icon, the About rewritten around it, and a look at Session launching: "when I played a scene, stopped the track and relaunched a single clip, it kept waiting for a sync; I had to stop and put the arrangement back at 0.0.0". Expected: Play on → a launch waits for the quantization point; Play off → it starts at once.

### The launch bug (`Transport::launch`, `Engine::apply` Locate)

- Launching from a standstill sent `Launch` first and `play()` after. The engine queued the launch at its current beat (`quantized()` with the song stopped returns `m_beat`), and `play()` then located to the start marker. Stopped once mid-song (the playhead stays where it was), the launch sat at that old beat and the clip flashed until the song got there; stopped twice (back to 1.1.1) the two beats coincided, which is why "back to 0.0.0" cured it.
- Now `Transport::launch` locates to the start marker *before* queuing when the song is stopped (the UI mirror's `queuedAt` is right too), and the engine moves every queued launch or stop to the new beat on a `Locate` while stopped, so the two can never disagree. Playing, nothing changed: a clip or scene waits for its quantization point (the clip's own, else the global one).

### The logo and the icon

- `assets/logo/sessions-logo.svg` is the full logo (the mark and the outlined wordmark, charcoal `#222221` on nothing, 1128 × 256), `sessions-logo-light.svg` the same in `#f2f2f0` for dark pages (the README's `<picture>` switches), `sessions-mark.svg` the S alone (320 × 320).
- The app icon (`assets/icon/sessions.svg`) is the mark at 1.15× on the charcoal tile the old icon had, each segment a colour with a light-from-above sheen: variant **A, ember**, the app's accent family from amber at the top to red at the bottom. `assets/icon/variants/` keeps all six treatments (A ember, B the old icon's hot / teal / violet, C six clip colours, D grey clips with one playing in the accent, E a paper tile, F a teal-to-violet dusk); to switch, copy one over `sessions.svg` and re-render the PNGs (`cairosvg` at 16, 32, 48, 64, 128, 256, 512).
- **Help → About** is a dialog of its own (`MainWindow::aboutDialog`): the logo rendered from the resource with its fill swapped for `theme::text` (so it reads on every theme), the version, the new description (the Session grid and the Arrangement, MIDI and audio, instruments and effects, Neural Amp Modeler), "Built with Qt". No other product is named anywhere in the app, the docs or the package any more.

### Tests (9 new; 999 in all)

- M24 launch: a scene from 1.1.1, Stop Track at the next bar, one Stop leaving the start marker at 1.1.1, then a single clip launched from the standstill plays within a tenth of a beat from 1.1.1 (the regression), a scene from a standstill with the start marker at bar 2 starts there in phase, and with the song playing a launch waits for the next bar and lands on it.
- M24 about: the dialog's logo renders from the resources and the text is the new one.

## Milestone 25: your icon, Preferences with a sidebar, clean buttons (done)

After 0.24: "preserve the other icons as well, and have an option in preferences for the user to set it to their liking · make the full logo in the About section follow the colour style chosen · redesign the menu and buttons, everything following the main UI's design · make the preferences a window with a side bar with all the categories and a main pane with the options (like Bitwig's) · no coloured icons on the buttons like OK, Save, Discard: a clean button with the UI's design".

### The app icon (`ui/AppIcon.h/.cpp`, `appicon::`)

- `appicon::variants()` is the table of the six treatments (key, name, a line about it, the SVG in the resources, the six segment colours; an invalid colour means "a grey clip": the ink in the logo). `currentKey()` reads QSettings `ui/icon` (ember by default; an unknown key is the default), `icon(key)` the QIcon, `applyWindowIcon()` sets the application's window icon at startup (`main.cpp`).
- `setCurrent(key)` saves, sets the window icon and writes the **desktop entry override**: `~/.local/share/applications/sessions.desktop` (XDG `ApplicationsLocation`) is the shipped entry (`:/sessions.desktop`) with `Icon=` pointing at the SVG written to `~/.local/share/Sessions/Sessions/Icons/sessions-<key>.svg`, plus an `X-Sessions-IconChoice=<key>` line; `StartupWMClass=sessions` stays, so the running window matches it. Choosing the default removes the override (only one carrying the marker). An absolute icon path needs no install, so a build-dir run works too. Some desktops read the override at once, some after a new login.
- `logo(ink, width, dpr, key)` renders the full logo (`:/logo/sessions-logo.svg`) with the root fill swapped for the ink (the wordmark) and a `fill` added to each of the six segment elements by id, so the About's mark is in the chosen colours (Playing: the accent and the ink).

### Preferences (`ui/Preferences.cpp`)

- A `QListWidget#prefsSidebar` (172 px, the categories: Audio, Record, MIDI, Look, Startup) drives a `QStackedWidget#prefsPages`; the page you were on is remembered (`prefs/page`). A page is a title (`#prefsTitle`) over rows: `row(label, control, hint, parent)` puts the setting's name in a 150 px left column (`#prefsRowLabel`), the control(s) and a dim note on the right, a hairline under (`QWidget#prefsRow`). The JACK box groups the ports as **Outputs** and **Inputs** rows; the PulseAudio box and the status (now a **Status** row) as before; the MIDI list is an **Inputs** row; Look has **Zoom** (0.26: it applies at once), **Zoom drag** and **App icon**: six `QToolButton#iconChoice` (icon 48 px, the name under, the chosen one outlined in the accent; property `iconKey`), a click is `appicon::setCurrent`. The footer (`#prefsFooter`) holds a plain **Close** (`#prefsClose`), no `QDialogButtonBox`. Every control keeps its object name (`audioSystem`, `audioFrames`, `jackPort0..3`, `audioDevice`, `audioBuffer`, `inputDevice`, `countIn`, `midiPorts`, `zoomDragDirection`), plus `uiZoom`, `rescanPorts`.

### Buttons, menus, message boxes (`Theme.cpp` stylesheet, `theme::installAppStyle`)

- `QDialogButtonBox { dialogbuttonbox-buttons-have-icons: 0; }` and the proxy style's `SH_DialogButtonBox_ButtonsHaveIcons` = 0: no desktop-theme icons on OK / Save / Discard / Cancel / Close anywhere. Dialog push buttons are a little roomier (`QDialog QPushButton`: 20 px, 64 px wide, 3 / 14 px padding), the default button has the accent border; pressed and disabled states for every button.
- `theme::installAppStyle()` (main and the smoke test) wraps the platform style in a `QProxyStyle` whose `standardIcon` draws the message-box glyphs flat: a disc in the accent (question, information), `theme::solo` (warning) or `theme::record` (critical) with ?, i or ! in `onAccent`, so a QMessageBox looks like the app rather than the desktop.
- Menus: the panel colour, 4 px padding top and bottom, roomier items, a disabled colour, separators inset; the menu bar's items rounded with a pressed state.

### Tests (13 new; 1012 in all)

- M25 icon: six variants and the default for an unknown key; choosing one saves it, sets the window icon and writes the desktop entry (icon path, `StartupWMClass`, the marker) and the SVG under a scratch `XDG_DATA_HOME`; the About logo's top-left segment takes the variant's colour and the wordmark the ink; Playing's grey clips take the ink; the default removes the override.
- M25 prefs: five categories drive five pages (no QTabWidget), the page is remembered, six icon buttons with the current one checked, a click changes the app's icon, a plain Close and the old controls by name; the stylesheet carries the no-icon rule and the sidebar; the proxy style answers the hint and draws the glyphs.

## Milestone 26: instruments land anywhere, the zoom applies at once (done)

After 0.25: "I can't drag an instrument like Monolith to a new track or empty space; it only allows me to drag it into the Device View. It needs to be as flexible as audio samples" and "the zoom should be done without restarting; besides, clicking Yes to restart restarts, but doesn't show the new zoom level until you close the app and start it again".

### Device drops (`DeviceDrag` `newTrackAfter`, `drop(…, after)`, `Project::insertTrackFor(blocks, name, after)`)

- The Session grid, the mixer strips and the Arrangement lanes already took devices (M11 / M21: on a track, the free space and the Master column make a new track, an empty audio track converts). What was refused with a "forbidden" cursor was an instrument or a MIDI effect over a track that can't take it and can't convert: an audio track with clips, a group, a return. Now `newTrackAfter` says so and the drop goes on a **new MIDI track right after that track** (`insertTrackFor` with `after`: the track is inserted at `after + 1` and joins that track's group), named after the device, holding it, one undo step. The Session grid shows an accent bar with "New track" at the column's right edge (`m_dropAfter`), the Arrangement a line under the lane (`m_deviceAfter`).
- The Arrangement's `deviceLaneAt`: anything in the lane area that isn't a track or the Master lane (the return lanes, below the Master) is a new track at the end; it used to refuse the return band.
- Moving device panels (`Payload::Devices`) keeps its rules: a panel refused by a track stays refused.

### The zoom (`main.cpp`, `MainWindow::restartReopeningSet`, View > Zoom, Preferences > Look > Zoom)

- The interface scale (`QT_SCALE_FACTOR`) is fixed when Qt starts, so a zoom change is a reload: View > Zoom and the Preferences' Zoom now apply at once, without the question. `restartReopeningSet` saves the window state first (the new instance opens where the old one was), snapshots the set as before (unsaved changes included), and starts the new instance through a `QProcess` whose environment has `QT_SCALE_FACTOR` and the marker removed.
- The bug: `main()` set `QT_SCALE_FACTOR` from the setting and the restarted instance inherited it, saw it set and left it alone, so the new zoom showed only after a second, manual start. Now the variable Sessions sets is marked (`SESSIONS_SCALE_FROM_SETTINGS`), and a marked one is always re-derived from the setting (unset at 100 %); a `QT_SCALE_FACTOR` you set yourself still wins. Checked offscreen: a child inheriting 1.25 with the marker and a 1.5 setting renders at 1.5; without the marker, 1.25 stays.

### Tests (6 new; 1018 in all)

- M26 drop: `newTrackAfter` for an audio track with a clip (not for a MIDI track); synthesized drag events on the Session grid's column and the Arrangement's lane land Monolith on a new MIDI track right after it, one undo step; `insertTrackFor(after)` joins the group; Preferences' Zoom has no Restart Now button.

## Milestone 27: next

- A note clipboard for the piano roll; the envelope editor showing the brace and ▶; Set 1.1.1 Here moving the markers with the audio.
- Monolith: a compact main panel for narrow screens, a free delay time, reverb pre-delay / damping, chorus feedback, unison up to 8 (a new choice parameter: the old one can't grow).
- Tempo: a downbeat finder (so dropped loops start on 1), the found tempo per sample cached in the browser (and shown there), async detection for long files, hand-checked on the user's own sample folders.
- NAM: FiLM and condition DSP, stereo captures, a middle Quality for three-size containers, latency compensation across the chain, async loading of big files; a "guitar rig" recipe with a NAM in the Sounds.
- Performance controls for every instrument (bend / wheel / pressure in the PolySynth scaffold, the Bass, the Sampler), polyphonic aftertouch, and recording them into clip envelopes (CC / pitch bend in MIDI clips).
- Racks: drag pads onto each other, per-pad sends / mute / solo, chains between racks, auto-select in the zone editor; key / velocity fades as gain.
- More inputs / outputs: per-track output routing, a cue mix for headphones.
- Multi-clip note editing in the piano roll.
- Warping: an **Envelope** control for Complex Pro, a transient rule for plain Complex, slice editing in Beats mode, a scroll bar under a long device chain.
- Info View text review; packaging beyond .deb: AppImage / Flatpak, Windows and macOS builds.

---

## Milestone 28: endless drags, typing into fields, dialog buttons that fit (done)

After 0.27: "fix the cropping of the button (Close without Saving) and verify other popups are clean · in Monolith the mod percentage is hard to scroll up or down, and double-clicking and typing a value overlays the new one over the old · verify every value field is draggable vertically · when the cursor reaches a screen edge it should jump to the other side and continue, an endless scroll; the BPM is at the top of the app, so dragging up gets stuck after a few BPM".

### The endless drag (`widgets/DragValue.h` `dragwrap::vertical` / `horizontal`)

- `dragwrap::vertical(widget, global, &landed)`: when a move's global position is at the screen's top or bottom edge, the pointer is warped (`QCursor::setPos`) to four pixels inside the opposite edge and `landed` says where; the caller then measures the next move from there, so the jump isn't distance. Away from the edge, or where the pointer can't be warped (Wayland), it returns false and the drag just stops at the edge. `DragValue` (and so `Knob`, `Fader`, `LevelFader`, the mixer, the control bar's tempo, the Clip box, every panel knob) measures drags in **global** coordinates now and calls it after every move; the Arrangement's header fields (`Drag::Field`, `m_fieldWrap`) and Monolith's two painted vertical drags (the Position / ring displays) do too.
- `DragValue::mouseMoveEvent` keeps the same sensitivity (`m_sens` pixels for the range, Shift ten times finer).

### Typing into a field (`DragValue::mouseDoubleClickEvent`, `setEditScale`, `editing()`)

- The editor is an opaque, frameless `QLineEdit#dragValueEditor` (its own stylesheet in the theme's field / text / accent colours; `autoFillBackground`), and `paintEvent` skips the value while it's up: device bodies style line edits transparent, so Monolith's amount showed the old value under the new one.
- `setEditScale(100)` on the Monolith route amounts: the editor shows 35 for 35 % and reads what you type back through the scale; reading takes the first number in the text, so "40 %", "-12.5 dB" and "+3" all work.

### Dialog buttons (`Theme.cpp` stylesheet)

- The 0.25 `QDialog QPushButton` rule had `min-width: 64px`; with the stylesheet style a min-width *is* the button's content width, so every dialog button was 94 px and "Close without Saving" (the GNOME platform's text for Discard) was cropped on both sides. The min-width is gone; padding 3 / 14 px and the 20 px height stay. Checked offscreen: the save box, Export Audio, Keyboard Shortcuts, the theme editor and the file dialog.
- The Export Audio dialog's **Tail** was a QSpinBox, the one value in the app that wasn't a drag field; it's a `DragValue` now (0..30 s, step 1, double-click to type).

### Tests (6 new; 1024 in all)

- M28 fields: a DragValue in a transparent-line-edit host: double-click shows an opaque editor reading 35 for 0.35 at scale 100, typing "40 %" reads back 0.4; a 40 px drag in global coordinates raises the value; `dragwrap::vertical` does nothing mid-screen and, where the platform warps, lands the pointer near the bottom from the top edge; a message box with "Close without Saving" gives every button its text's width plus padding.

## Conventions for future iterations

- Themes (0.19): a view reads `theme::x` and never a literal, because `theme::apply(const Theme&)` writes every entry from a base, a tint, an accent and overrides; a new entry is a grey (tinted and stretched) unless `isTintable` / `isAccentColour` say otherwise, and a new signal colour goes into `isTintable`'s fixed list. Styles built from colours in device bodies go through `col::label` / `col::value`, never a hex.
- One colour per concept, all in `Theme.h`'s table with a Dark, Mid and Light value; read `theme::x` when you paint (a file-level `const QColor&` alias is fine, a `const QColor` copy is not: it would freeze one theme). No hex literals in views unless they're truly local shading (gradient stops, the guitar pedals' hardware colours). Selection outlines, focus rings and playheads are `theme::outline` / `theme::playhead`, never `Qt::white`; numbers in fields are `theme::readout`. A widget that builds its own stylesheet from colours rebuilds it on `theme::notifier()->changed()`. Keep Dark pixel-identical: when you add an entry, its Dark value is the literal it replaces (diff `--screenshots` captures against the previous build).
- A mouse gesture that has to outlive a view switch (the Session clip drag through Tab) is handed over explicitly: the widget that had it forgets it, the one that takes it `grabMouse()`s and releases on the button-up. Route every view switch through `MainWindow::showSession` so the hand-over happens whichever way the view changes.
- Custom-paint anything grid-like (`ClipGrid`, `ArrangementView`, `PianoRoll`); use real widgets for controls that need focus and keyboard behaviour (`DragValue` family, buttons).
- Attach hover help to every new control with `InfoView::attach(widget, "Title", "What it does.")`.
- Every edit to anything in `EditState` goes inside an `EditScope` (or `beginEdit`/`endEdit` around a drag), and emits the matching `Project` signal so the engine republishes. An edit outside a scope is silently reverted by the next undo.
- Refer to tracks by `Track::id` (and clips by `Clip::id`) anywhere that outlives one event: engine commands, recording, menus.
- Controls that can be automated: call `Project::touchTarget` when the user moves them, and show `Envelope::valueAt(transport->beat())` when automated and not overridden.
- New devices: add a `DeviceType` and a row to `DeviceSpecs.cpp` (at most `kMaxParams` = 192 parameters) (parameters as `ParamSpec`s), a processor in `Instruments.cpp` / `Effects.cpp` / `Devices.cpp` and its factory case, and a body in `src/ui/devices/` (every parameter needs a control; the panel test checks). Append parameters, never reorder or repurpose them. Nothing on the audio thread may lock, allocate or touch `Project`.
- Writing to the model inside an `EditScope`: look things up *after* the scope opens. A pointer or reference into Qt containers taken before it can write into the undo snapshot (they share data until written), and the edit silently vanishes from undo.
- Add a line to `tests/smoke.cpp` for every new interaction or engine behaviour, and keep `--screenshots` capturing each view.
- Anything new in `EditState` (or a new saved setting) also goes into `SetFile.cpp`, both ways, and into the M4 round-trip test, which compares `captureEditState()` after a save → load.
- Signals from objects the window owns (project, transport, undo stack) that call `MainWindow` members must be disconnected in `~MainWindow`; children outlive its destructor body.
- New controls people would want to play from a controller: give them a `mapTarget` property (and teach `Mapper::setValue` / `trigger` the target if it's a new kind).
- Check `Track::type` for anything MIDI-only or audio-only (`Project::acceptsDevice` for devices). Audio files go through `samples::load` (shared, cached) and into the set through `src/core/Import.*`.
- Engine tests that start a fresh `Engine` hear its ~10 ms gain ramp from silence: don't expect full level in the first milliseconds.
- An audio track that monitors its input (Monitor In, or Auto while armed) plays no clips: engine tests that render clips on an armed track must disarm it (or set Monitor Off) first.
- A device that needs another track's audio reads it from the engine's key buffers (`Context::keyL/R`), which `publish()` fills by ordering the tracks; anything else that must run after another track should go through the same order (`RenderState::order`), never by re-rendering.
- Audio takes map beats to capture frames through `AudioTake::linear` (the beats the loop has wrapped since the capture began); set it whenever a take's `startBeat` is set.
- A device that needs a heavy prepared resource (a capture's network, an IR's spectra) builds it on the UI thread in `Engine::publish`, caches it by device id (or by sample) so edits don't rebuild it, keeps it alive through the `RenderState` and hands it to the processor through `Context`; the audio thread only runs it. Files behind devices (`samplePath`, `modelPath`) are saved with `writePath` / `readPath` (absolute plus relative to the set), load to null when missing (the device plays dry, the panel says so in `theme::devMissing`), and go through Collect All and Save.
- DSP that implements a published format or algorithm is checked against a reference implementation when one is at hand (FORM's `vendor/` holds NeuralAmpModelerCore and its example models, read only): build the reference in the scratchpad, feed both the same signal, compare sample for sample, and say so in this file. Hot loops stay plain C++ (input-major weights, frames innermost, no reductions) and get `target_clones` rather than `-march` flags, so the .deb stays generic.
- Code that walks a track's devices as blocks (a device, or something holding chains and its chains' devices) uses `deviceBlockEnd` / `holdsChains`, never `isRack`: a top-level Drum Rack holds its pads' chains too. `isRack` is for what only the three racks have (macros, chain names, zones). A new chain goes through `appendChain` (name, spec-default knobs, default zones); anything that renumbers chains goes through `remapChainKnobs` so knob values, automation, clip envelopes and macro mappings follow.
- Session launches from a standstill take every track (`SessionTakeover`); anything that starts the transport on the Session's behalf goes through `Transport::launch`, not `play()`, so it keeps that rule.
- Controls a keyboard sends besides notes (bend, wheel, pressure) travel as `Command::Perform` per track and arrive in `Context`; an instrument that wants them reads `ctx.bend / wheel / pressure` every block (never caches them across notes).
- Monolith parameters and sources are appended, never inserted: `kParams` and `kSources` only grow; a new Source gets a name, a bipolar flag and a chip; a new parameter gets a control on the main panel or in a drawer (the panel test checks) and a `sectionName`. FX knobs (`>= kFxBase`) are modulated by the shared sources only (`globalMods`); a per-voice knob by `modulate`.
- A panel that animates something the processor knows (an envelope's position, an LFO) reads it through `DeviceBody::scope(k)` from `DeviceProcessor::scope`, written on the audio thread; a body whose width changes at run time (a drawer) just sets its fixed width: `DevicePanel` follows through its event filter.
- Audio clips are born in sync: anything that makes one goes through `Project::newAudioClip` (which runs `tempo::syncTo`), and anything that changes the set's tempo goes through `setTempo` inside the caller's EditScope (it re-matches warped clips). A clip whose markers must stay put in a test sets `detectedBpm = 0`. New fields a clip keeps go into `operator==(const Clip&)` and the set file.
- A pedal's clipper is written once as a memoryless shaper (`odClip`, `ratClip`, …) used both inside the oversampled loop and by `DrivePedal::transfer`, so the panel's graph is the sound; its filters live in `designFilters`, which `response` draws. New pedal parameters are appended (the rig recipes map knobs by index).
- Files behind devices that come built in (drum sounds, cabinet IRs) are `builtin:` paths through `audio::builtinSample`; a new kind gets a prefix below `builtin:`, a name, and a renderer there, and then loads, saves, drags and lists like a file.
- A new drum-style groove goes into `Grooves.cpp`'s table as lanes (16 steps per bar per lane, the step letters in `Grooves.h`); the test checks every lane's length and every note's pitch, so a typo fails the build's tests rather than playing wrong.
- Zoom-by-drag goes through `PreferencesDialog::dragZoom(dy)` (never a literal `exp(dy · k)`), so the preference's direction holds everywhere; a strip that scrolls on a sideways drag makes the thing under the pointer follow the pointer.
- A new warp mode that keeps the pitch goes into `audio/Stretch.*` as a `StretchMode` (its state in the `Stretcher`, never allocating after construction, restarting on a time jump), is mapped in `Engine::clipRef`, named in `warpModeName`, saved by word in `kWarps`, and gets its controls on the Clip View's row under the chooser.
- A clip's name bar is for moving it, its body for selecting inside it: anything new that reads a press on a clip keeps that split.
- A clip's position at play time is never `fmod(elapsed, length)`: a launched clip plays from `Clip::start`, a placement from its `offset`, into the brace `[loopStart, loopStart + length)` (0.23). Go through `clipPositionAt` / `forClipPasses` (engine, ops) or `Clip::positionAt` / `positionFrom` / `Project::clipPosition` (views), and scale or shift `start` and `loopStart` with the length whenever you scale or shift a clip's grid.
- The Sampler (0.32): its parameters are `sampler::` in DeviceSpecs.h (append, never reorder); anything that lists the slices goes through `sampler::slices()` (no allocation: the processor calls it every block, the panel draws the same list); its tempo and manual slices are `Device::sampleBpm` / `slices` (saved, in `operator==(Device)` and the M4 round trip); the panel's named controls are what the tests find.
- Update this file: move items from "next" to "done", add new gaps as you find them.
