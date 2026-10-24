# Nektar Impact LX49+ / LX61+

Sessions 0.49 supports the dedicated **Mixer** and **Inst** presets as well as **User Preset 2**. Enable the keyboard's MIDI inputs in **Preferences → MIDI**, leaving **Profile → Automatic** selected. Named LX49+/61+ ports are detected automatically; choose a specific controller input if its port has been renamed. User MIDI Maps take priority.

## Dedicated Mixer / Instrument setup

Set the keyboard's **USB Port Setup to 0**. Nektar's documented procedure for the LX49+/61+ is:

1. Hold **Shift/Mute** and press **Patch > (Setup)**.
2. Press **A2** (the 13th white key from the left).
3. Press **C3 (−)** until the display shows **0.0.0**.
4. Hold **Shift/Mute** and press **Patch > (Setup)** to exit.

Press **Mixer** or **Inst** on the keyboard. These read-only presets send the control messages on MIDI channel 16; the global keybed channel does not need changing for these modes. Enable both MIDI input ports if they are listed separately.

| Hardware | Sessions action |
| --- | --- |
| Mixer faders 1–8 | Track-bank volume |
| Mixer knobs 1–8 | Track-bank pan |
| Mixer buttons 1–8 | Toggle track mute |
| Fader 9 | Master volume |
| Button 9 | Arm/unarm the selected track |
| Track − / + | Previous/next bank of eight tracks |
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

The native mode uses the fixed channel-16 assignments published in the Nektar/Ardour controller map. It does not load Bitwig's integration package or negotiate its proprietary modes. Instrument fader envelope layouts, Patch/Page navigation, Grab assignments and pad/display/LED feedback are not implemented. Keys, pads, wheels and sustain retain normal instrument/MIDI Map behavior. Select **Profile → Off** to disable the global profile.

Regression tests inject the dedicated and GM messages, exercise pickup, banks, device selection, buttons, transport and mapping precedence. Physical LX49+ verification of the new dedicated mode is still required.

References: [Nektar's Ardour/Mixbus setup and control layout](https://support.nektartech.com/using-harrison-mixbus-or-ardour-with-impact/), [published LX MIDI assignments](https://github.com/Ardour/ardour/blob/master/share/midi_maps/Nektar_Impact_LX.map), [Nektar's Bitwig integration](https://support.nektartech.com/download/using_impact_lx_plus_with_bitwig/).
