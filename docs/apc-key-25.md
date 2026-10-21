# Akai APC Key 25 (2014) and APC Key 25 mk2

Sessions 0.45 has a built-in profile for both APC Key 25 models. The pads launch Session clips, the buttons work the way they're printed, and the pads light up to show the clips. The keys play the armed (or selected) track as before. Sessions finds the controller by its port name; there is nothing to install.

## Setup

1. Connect the APC and open **Preferences → MIDI**. Leave its input ports checked.
2. Leave **Control surface → Profile → Automatic** selected. **Off** turns the profile off, so the pads play notes again.
3. A red outline appears on the **Session View**. That's the **box**: the 8 tracks × 5 scenes under the pads. The top row of pads is the box's first scene.

The mk2 shows up as two ports. The first carries the keys and the sustain button. The second carries the pads, buttons and knobs, and receives the lights. Sessions plays every note from the first and none from the second. The 2014 model has one port: its pads and buttons use channel 1, and its other notes play.

## Pads and buttons

| Control | Sessions action |
| --- | --- |
| Pad | Launch the clip (its launch mode applies; letting go releases a Gate clip). On an empty slot: stop the track, or record a new clip if the track is armed. A group's pad launches its tracks' clips in that scene. |
| Shift + pad | Show the clip in Clip View without launching it |
| Scene launch 1–5 | Launch the box's scenes |
| Stop All Clips | Stop every clip |
| Play | Play / stop |
| Rec | Arrangement Record on / off |
| Track buttons 1–8 | The printed mode: **Clip Stop** (to begin with), **Solo**, **Rec Arm**, **Mute** or **Select**, for the box's 8 tracks |
| Shift + scene button | Choose the track buttons' mode (the labels under the scene buttons; the 2014 model has Rec Arm third and Mute fourth, the mk2 the other way round) |
| Shift + track buttons 1–4 | Move the box up / down / left / right (one scene or track at a time; it stops at the set's edges) |
| Shift + track buttons 5–8 | Knobs: **Volume**, **Pan**, **Send** (press again for the next send), **Device** |

## Knobs

- **Device** (to begin with): the selected device on the selected track. Osmium, racks and Macros turn **Macro 1–8**. Other devices turn their first eight parameters. With no device selected, the track's instrument is used.
- **Volume / Pan / Send**: the box's 8 tracks.
- The mk2's knobs are endless. Each step moves the value by 1/127, and **Shift** turns finely. The 2014 model's knobs are absolute. With **Pickup** on, a knob takes over when it reaches the value on screen.
- A **MIDI Map** you make takes priority over the profile. A knob mapped from an endless controller is recognised while you map it, and steps the value instead of jumping to the ends. Maps of the mk2's knobs made before this change step as well.

## Lights

| Pad | mk2 | 2014 model |
| --- | --- | --- |
| Clip, stopped | the clip's colour | amber |
| Playing | green, pulsing with the beat | green |
| Queued | green, blinking in time | green, blinking |
| Recording | red, pulsing | red |
| Empty, armed track | dim red | dark |
| Empty | dark | dark |

Track buttons light for what they'd change: playing tracks (Clip Stop, blinking while a stop is queued), soloed, armed, playing (Mute lit = track on, as in Live) or selected. Scene buttons light for scenes with clips and blink while one is queued. While **Shift** is held, track buttons 1–4 show which ways the box can move, 5–8 the knob mode, and the scene buttons the track-button mode. The lights go out when Sessions closes or the profile is switched off.

## Scope and verification

This profile is implemented by Sessions from Akai's published mk2 protocol and Ableton Live's layout for both models. It sends no firmware or mode commands. Blinking and pulsing are drawn by Sessions in time with the song, so no MIDI clock is needed. Regression tests (`sessions_apc_key25`) inject both models' messages and check every action, light and port rule. Physical hardware has not been tested yet; check the pads and lights once the controller is connected.

Public references: [Akai APC Key 25 mk2](https://akaipro.com/apc-key-25-mkii), [Akai support](https://support.akaipro.com/en/support/solutions/articles/69000826281-akai-pro-apc-key-25-mk2-frequently-asked-questions).
