# Ableton Live parity audit · Sessions 0.52.0

Checked 1 November 2026 against the current official Live 12 manual and edition matrix, and Sessions' source. This is a capability comparison, not a claim of identical sound, performance or full compatibility. Live editions differ; device comparisons include Suite and Max for Live where indicated. No Ableton code, presets or artwork were copied.

## Recording and arrangement

The user's chosen priority for this build is recording and arrangement. Status describes the completed 0.52 build; the prior public release is 0.51.0.

| Capability | Sessions status | Practical difference / next work |
| --- | --- | --- |
| Session clips and scenes; linear Arrangement | Present | Separate live-launch and timeline playback, with Back to Arrangement. |
| Record Session performances onto the timeline | Present | Audio/MIDI placements and clip changes are recorded. |
| Hardware, track, group and Master audio recording | Present | Internal inputs use stable track IDs; monitoring cycles are blocked. |
| Count-in, punch in/out, loop recording | Present | Hardware recording has input/output latency correction. |
| Audio takes and comping | Present | Comp regions and crossfades live inside an audio clip. No linked multi-track take editing or MIDI take-lane comping. |
| Retrospective MIDI capture | **Added in 0.52** | Last phrase on the selected MIDI track; Session free slot or Arrangement placement. No tempo inference or captured expression. |
| Print an Arrangement track/group or time range | **Added in 0.52** | Bounce to a new audio track, retaining the original. Includes track effects, pan, fader and automation; no return effects baked in. |
| Aligned individual track, group and return exports | **Added in 0.52** | Same start, sample count and tail; independent returns and a timing manifest. Child stems exclude parent-group/Master processing. |
| In-place bounce / processor freeze | Missing | Only new-track Arrangement bounce is added here. No Session bounce or processor-suspending cache. Live’s current in-place bounce supersedes its earlier Freeze/Flatten command. |
| MIDI overdub into an existing playing clip | Missing | Current recording creates new clips. Add safe note merge/replace, loop and held-note handling. |
| Recording performance expression | Partial | Bend/wheel/pressure play instruments live; they are not recorded into clips. Per-note MPE also needs event/model/editor work. |
| Split, consolidate and ripple time edits | Present / partial | MIDI consolidation, range cut/copy/paste, insert/delete/duplicate time and locators exist. Audio consolidation still needs an in-place render operation. |
| Clip and group fades | Present | Placement fades with editable lengths/curves; not linked comp lanes. |
| Automation | Partial | Device/mixer envelopes, clip envelopes, overrides and re-enable exist. Live controller gesture recording and tempo/time-signature automation remain gaps. |
| Latency compensation | Partial | Hardware-recording alignment and local wet/dry alignment exist. No complete host-wide effect-latency graph. |
| Export formats | Partial | Stereo WAV, 16/24-bit or float, fixed 48 kHz. No selectable export rate, compressed formats or surround export. |
| External device / plugin integration | Missing / partial | MIDI hardware profiles exist; no VST/AU hosting or general external-instrument/effect device with round-trip compensation. |

Ableton reference: [Bounce to Audio](https://www.ableton.com/en/live-manual/12/bounce-to-audio/) (Live uses a pre-mixer bounce and preserves mixer settings; Sessions prints post-mixer), [Comping](https://www.ableton.com/en/live-manual/12/comping/), [Recording New Clips](https://www.ableton.com/en/live-manual/12/recording-new-clips/), [Arrangement View](https://www.ableton.com/en/live-manual/12/arrangement-view/), [Managing Files and Sets](https://www.ableton.com/en/live-manual/12/managing-files-and-sets/), [Computer Audio Resources and Strategies](https://www.ableton.com/en/live-manual/12/computer-audio-resources-and-strategies/). The [edition matrix](https://www.ableton.com/en/live/compare-editions/) still lists Track Freeze alongside Capture MIDI and MPE; the current manual documents the newer bounce workflow. Similar feature names do not establish full implementation parity.

## Devices and editing beyond this build

Sessions already has 63 primary devices, including racks, key/velocity zones, FM and wavetable synthesis, single/multisampling, recorded drums, parametric EQ, spectrum analysis, sidechains, multiband dynamics, modulation mapping, pitch correction/shifting, hybrid convolution reverb and a performance looper. These families are implemented, not absent simply because names differ.

| Area | Remaining gaps worth considering |
| --- | --- |
| Creative audio effects | Beat Repeat, Grain Delay, a dedicated Vocoder, spectral resonator/time processing and modulated multiband distortion. Existing Delay, Redux, saturation and pitch effects cover only parts of these workflows. |
| Physical-model instruments | No dedicated bowed/string/body modelling comparable to Tension or Collision. Electric and Drum Studio overlap only part of the use cases. |
| MIDI effects | Arpeggiator, Chord, Scale, Velocity and racks exist. Dedicated note-length, random pitch, MIDI pitch and CC-control devices are absent. |
| Note tools | Basic transpose, reverse, inversion, legato, stretch, scale display and probability exist. No full transformation/generator panel with strum, chop, rhythm and chord generators. |
| Ecosystem | No Max for Live runtime, Push equivalence, Ableton set import, Link/Link Audio or stem-separation service. Existing Nektar/APC integrations are narrower controller profiles. |

References: [Live Audio Effect Reference](https://www.ableton.com/en/live-manual/12/live-audio-effect-reference/), [Live Instrument Reference](https://www.ableton.com/en/live-manual/12/live-instrument-reference/), [MIDI Effect Reference](https://www.ableton.com/en/live-manual/12/live-midi-effect-reference/), [MIDI Tools](https://www.ableton.com/en/live-manual/12/midi-tools/), [Max for Live Devices](https://www.ableton.com/en/live-manual/12/max-for-live-devices/).

## Next recording milestones

1. Reversible track freeze with persisted audio, original state, cache invalidation, Session support and routing/dependency tests.
2. MIDI overdub and recorded controller expression, then per-note MPE and editing.
3. Linked multi-track audio takes/comping for drums and multi-microphone recording.
4. Host-wide latency compensation, with branch alignment through groups, returns and sidechains.
5. Audio consolidation, selectable export sample rates and tempo/time-signature automation.

0.52 instructions and exact limits: [Recording workflow](recording-workflow.md). Evidence is in the model, Transport, Arrangement operations, engine routing and the focused `recording_workflow` regression suite; hardware listening tests remain separate.
