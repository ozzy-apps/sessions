# Nektar Impact LX49+ / LX61+

Sessions 0.38 includes a native input profile for the LX49+ and LX61+. It uses the keyboard's public GM mixer controls and standard MIDI Machine Control (MMC). Sessions detects matching MIDI input names automatically; an explicitly selected input also supports renamed ports.

## Setup

1. Connect the keyboard and open **Preferences → MIDI**. Enable its input ports.
2. On the keyboard, select **User Preset 2 (GM Mixer 1–8)** and set **Global MIDI Channel to 16**. This keeps the ninth fader separate from the eight fixed-channel faders. The Mixer/Instrument buttons used by manufacturer DAW packages select different assignments; this Sessions profile uses the user preset.
3. Leave **Profile → Automatic: Nektar Impact LX49+ / LX61+** selected. Input **Automatic** restricts the profile to named LX49+/61+ ports; choose a particular input if the operating system presents another name.
4. Choose the track bank. **Pickup** prevents jumps: move a fader or pan knob through the current on-screen value before it takes control. Changing banks, undoing or editing a value on screen re-arms pickup. Disable Pickup for immediate absolute control.

| Hardware | Sessions action |
| --- | --- |
| Faders 1–8 | Track-bank volume |
| Knobs 1–8 | Track-bank pan |
| Buttons 1–8 | Mute/unmute the corresponding track |
| Fader 9, Global channel 16 | Master volume |
| Keys, pads, wheels, sustain pedal | Normal instrument input; custom MIDI Maps remain available |

Tracks in a bank include group tracks. Group faders therefore control their complete buses. Empty positions do nothing. Custom set-specific MIDI Maps take priority over the profile and survive bank/profile changes. The global profile does not add mappings to the set.

## Transport

Sessions accepts complete MMC messages from the selected controller. If necessary, program the keyboard's transport buttons to the **MMC** assignment type and the following Data 2 values. These are standard command numbers, not a manufacturer handshake.

| Button | MMC Data 2 (decimal) | Sessions action |
| --- | --- | --- |
| Stop | 1 | Stop without rewinding on repeated messages |
| Play | 2 | Continue playback; repeated messages keep playing |
| Forward | 4 | Move forward four beats |
| Rewind | 5 | Move back four beats, bounded at zero |
| Record | 6 | Enable Arrangement recording and play |
| Record exit | 7 | Disable recording, keeping playback running |

Loop/metronome/scene controls can use **MIDI Map** to learn an assignable CC button or pad. Default pads continue to play notes; the profile does not take them over for launching. The computer keyboard's number row **1–9, 0** launches scenes 1–10.

## Scope and verification

This profile is implemented by Sessions. It does not install Nektar's DAW package, change controller firmware, negotiate a proprietary operating mode or send pad/display/LED feedback. Physical hardware has not yet been verified; regression tests inject named MIDI input, validate packet handling, mixer pickup, custom mapping priority and transport actions. Once the keyboard is connected, check each bank and transport button before a performance. Select **Profile → Off** to retain ordinary MIDI input and user maps alone.

Public references: [Nektar LX49+/61+ overview](https://nektartech.com/impact-lx49-61-plus/), [Nektar support](https://support.nektartech.com/impact-support/), [MIDI universal messages](https://midi.org/midi-1-0-universal-system-exclusive-messages).
