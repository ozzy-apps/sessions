# Nektar Impact LX49+ / LX61+

Sessions 0.50 supports native pad/LED feedback, the dedicated **Clips/Scenes** modes, and the **Mixer** and **Inst** presets as well as **User Preset 2**. Enable the keyboard's MIDI inputs in **Preferences → MIDI**, leaving **Profile → Automatic** selected. Named LX49+/61+ ports are detected automatically; choose a specific controller input if its port has been renamed. User MIDI Maps take priority.

## Native pad and LED setup

1. Enable both **Impact LX49+ MIDI1** and **MIDI2** inputs in **Preferences → MIDI**.
2. Keep **Profile → Automatic** and **Pads → LED feedback** enabled.
3. Press **Clips** to launch the selected track's clips, or **Scenes** to launch scenes across the set. The matching button lights up. Press the active mode again, press Preset, or choose **Notes** in Preferences to return to normal pad playing.
4. Hold **Clips** or **Scenes** and press pad 1–8 to choose a bank. Each mode remembers its own bank, covering up to 64 existing scenes. Track −/+ follows the selected track in launcher modes. Shift + Track −/+ keeps the mixer-bank action.

Loaded slots are **yellow**, playing clips **green**, recording **red**, and empty slots **off**. Queued launches blink; playing scenes pulse with the beat. Bank selection uses **orange** for the current bank. An empty slot starts recording when its track is armed, and otherwise stops its track. Group pads control the group's member tracks. Shift-pad selects a clip without launching it. Keys and ordinary pad notes on MIDI1 continue to play instruments; custom MIDI Maps take priority.

Sessions automatically selects USB Port Setup 2 for the paired native input/output and restores 0 when feedback is disabled or Sessions closes. It preserves stored pad maps and user presets. A renamed or single-port input continues to work with the basic input profile; native feedback requires the named pair. Only one DAW should control this integration at a time.

## Basic Mixer / Instrument setup

With LED feedback disabled, use **USB Port Setup 0**. To set it manually: hold **Shift/Mute** and press **Patch > (Setup)**, press **A2** (13th white key), use **C3 (−)** until **0.0.0**, then leave Setup the same way.

Press **Mixer** or **Inst** on the keyboard. These read-only presets send the control messages on MIDI channel 16; the global keybed channel does not need changing for these modes. Enable both MIDI input ports if they are listed separately.

| Hardware | Sessions action |
| --- | --- |
| Mixer faders 1–8 | Track-bank volume |
| Mixer knobs 1–8 | Track-bank pan |
| Mixer buttons 1–8 | Toggle track mute |
| Fader 9 | Master volume |
| Button 9 | Arm/unarm the selected track |
| Track − / + | Selected track in launcher mode; mixer bank in Notes mode or with Shift |
| Inst knobs 1–8 | First eight parameters of the selected device; labelled Macro 1–8 on racks |
| Cycle | Toggle loop |
| Rewind / Forward | Move back/forward four beats |
| Stop / Play | Stop/continue playback |
| Record | Toggle Arrangement recording |

Device knobs follow the selected device in Device View or its expanded editor. With no explicit device selection, they target the selected track's instrument, or its first effect. Tracks in a bank include groups. Empty positions do nothing. Pickup applies to mixer faders, pan and device knobs: move through the current value before taking control. Switching banks/devices or editing a value on screen re-arms pickup.

## User Preset 2 and MMC

The previous GM preset remains supported. Select **User Preset 2 (GM Mixer 1–8)** and **Global MIDI Channel 16** to keep fader 9 separate. Faders 1–8 use CC7 on channels 1–8, knobs use CC10, and alternating mute buttons use CC12. Fader 9 uses CC7 on channel 16.

MMC Stop (1), Play (2), Forward (4), Rewind (5), Record (6), Record Exit (7) and Pause (9) remain accepted. MMC Record enables recording and starts playback; the dedicated Record CC toggles the record button.

## Scope and verification

The native feedback implementation uses the LX+ wire protocol checked against the controller extension bundled in Bitwig's official installer; Sessions builds its own messages and launcher state. It does not load Bitwig's integration code. Instrument fader envelope layouts, Patch/Page navigation, Grab assignments, automatic preset maps and display feedback are not included in this pad update. Select **Profile → Off** or disable **LED feedback** to restore basic operation.

Regression tests cover the dedicated and GM messages, pickup, banks, device selection, transport, source-scoped pad feedback, output retries and mapping precedence. Physical LX49+ verification of the new colours and buttons remains necessary. [Protocol and delivery details](nektar-feedback-protocol.md).

References: [Nektar's Ardour/Mixbus setup and control layout](https://support.nektartech.com/using-harrison-mixbus-or-ardour-with-impact/), [published LX MIDI assignments](https://github.com/Ardour/ardour/blob/master/share/midi_maps/Nektar_Impact_LX.map), [Nektar's Bitwig integration](https://support.nektartech.com/download/using_impact_lx_plus_with_bitwig/).
