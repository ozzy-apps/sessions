# Sessions 0.23 — things to try by hand

## 0.31 · Drum Buss

1. Browser → Audio Effects → Drum Buss, after Drum Studio or on a group/audio track. Check the compact panel in Dark/Light and custom themes, all controls at 100/150% UI scale, and the Bass/Out meters during playback.
2. Drive: compare Soft, Medium and Hard at 20–80%. Hard emphasizes bass; Medium limits peaks; Soft rounds them. Turn Comp on and compare loud/soft hits. Try Trim before Drive, and match level with Output.
3. Crunch adds mid-high texture; Damp removes harshness. Set Transients positive for fuller hits, negative to tighten room/rattle. The low bass should remain while the high-band envelope changes.
4. Increase Boom on a kick loop. Try Freq 40/55/80Hz, short/long Decay, and Bass Solo. Force to Note snaps the resonator; the displayed pitch/frequency follow. With Boom at 0, shorter Decay still tightens the incoming low end. Silence should not excite the resonator.
5. Use Dry/Wet for parallel processing and Output to leave headroom. Type +6 into Trim (it means +6dB), -6 into Output, 50 into Dry/Wet (it means 50%). Delete resets each field. Try undo/redo and MIDI mapping/automation for knobs, type buttons and switches.
6. Save/reopen a set and an effect preset with nondefault tuning/solo/mix. Controls and sound return. Bypass, stop/panic, and change buffer/sample rate; check for switching clicks or remaining resonator tails after a panic.

## 0.30 · Recorded Drum Studio and embedded instruments

1. Browser → Drums → Drum Studio → **Expand**. The editor opens inside the bottom device pane; drag its divider. **Device chain ▾** returns to the compact rack. Check both instruments at a smaller window size and in Dark / Light / custom themes.
2. Try all six kit recipes. Studio Rock / Tight Pop use Black Pearl recordings; Vintage / Metal / Big Room use Red Zeppelin; Jazz uses Blonde Bop. Play ghost notes and accents: both the timbre and volume change. Try fast snare and double-kick patterns; tails overlap without repeated notes cutting each other off.
3. Click each drum. Audition snare Center / Rimshot / Side stick / Clap, hats Closed / Pedal / Open, ride Bow / Bell / Shank, crashes / splash / china. Close a ringing open hat. Adjust Tune, Decay and Tone; Tune keeps its meaning while Decay changes damping/sustain.
4. Mixer: drag each fader, Pan, Mute / Solo; undo and redo. Check that channels also leave the ambience buses when muted. Try overheads, room, size, bleed and width. These are modeled ambience controls.
5. Grooves: select DnB, Techno, Rock, Metal, Reggae and other categories. Search a name; clear the search. Preview loops at the set tempo without starting transport or making a clip. Leaving Grooves, collapsing or changing tracks stops preview.
6. Adjust Velocity / Timing humanization. Drag a groove onto a Session slot or Arrangement lane; **To Clip** and double-click insert an editable clip in the next empty slot. Undo removes it. Verify the loop length and ghost notes in the piano roll.
7. **Export MIDI…** produces a .mid with GM channel 10, tempo, 4/4 and exact loop end. Open it in another MIDI editor. The downloadable bank also includes .sessionsclip versions for Sessions’ User Library.
8. Osmium → Expand: Sound / Modulators / Voice & mixer / Effects and all macros/range controls remain accessible. Voice & mixer stays selected while editing Detune or its range. Undo, preset loads and theme changes preserve the embedded editor; deleting the device closes only the editor.
9. Install the fresh .deb and audition the kits away from the source checkout. No external sound library, network access or sampler plugin is required. Listen on monitors/headphones at your normal audio buffer and adjust kit levels to taste.

## Osmium DnB bank and editor pages (0.29)

1. Open Osmium's patch selector, type part of a name (for example `seismic`) → the matching sound is offered. There are 40 new sounds: eight each of Pad, Lead, Sub, Reese and Wobble; the original starting patches remain.
2. Search `Osmium` in Browser → Sounds and load a sound → the editor's name, parameters, routes, both curves and eight macro labels match the selected factory patch. Undo/redo a patch load → the previous/next sound returns in one step and the editor stays open.
3. Start on Curve 1, switch to Voice & mixer and assign a Curve 1 range to Detune. Select another source, return to Voice & mixer, drag Detune's centre and green ring → Voice & mixer stays visible; the source highlight may change to the ring's source. Repeat on an Effects knob. Clicking a source chip explicitly still opens its editor.
4. Hold open chords for the pads, play leads around MIDI 55–79, and try basses around MIDI 28–43. Turn the macros: Tone, Warp, Drive, Width, Space, Echo, Body, Air → all eight change the sound. The single-voice Glasswire Width macro and sub Width macros add chorus; default subs stay centered and dry.
5. Play the wobbles with the project at 170/174 BPM → their curves, LFOs and stepper follow the tempo. Try short syncopated notes and longer held notes, then glide between overlapping notes.
6. Save a modified factory sound as a user preset and save/reopen the set → the curves, envelope bends, routes and macro positions return. Unzip the portable bank and drag a `.sessionspreset` onto a MIDI track → it also plays in an existing Osmium-capable 0.27/0.28 build.

The automated tests can't plug in a keyboard, sing into a microphone or tell whether something sounds good, so this list is everything since 0.5 that needs your hands and ears. Each line says what to do and, after the →, what should happen. Note anything that doesn't, and anything that sounds wrong (clicks, crackles, noise, too loud or too quiet, a delay you can feel).

## 0. Start

1. Install the newest `.deb` from `dist/` (`sudo apt install <full path>`) and start Sessions from the app menu → the demo set opens; Help → About says 0.18.
2. Press Space → the demo plays; Space again stops it.

## Osmium (0.27)

1. Load Instruments → Osmium onto a MIDI track and click Expand → a floating synth opens; the rack remains compact. Play a keyboard or press Audition C2 → sound comes through the track.
2. Env 2 initially controls Filter 1 Cutoff. Drag Cutoff's green ring → the range changes and its base stays put. Drag the centre → the base changes. Shift → fine movement. Inspect the reachable Hz values on the right.
3. Select LFO 1 and click Cutoff's + / source slot → both sources control the same parameter. Drag its range below zero → modulation reverses. Double-click the range amount and type -42.5 → the amount reads -42.5%.
4. Drag Curve 1 onto Position → Curve 1 becomes selected and its ring appears. Use Remove, undo and redo → the assignment and its signed amount return.
5. Drag an envelope's peak, decay and release nodes → the times and sustain controls follow. Drag each diamond → only that segment's bend changes. Hold a note while editing and listen for the contour; toggle Loop.
6. Curve 1: add nodes with double-click, move them, drag the segment diamonds, choose Smooth / Hold, change snap and hold Alt. Try Draw with a fast diagonal sweep → no skipped grid positions. Each drag undoes in one step.
7. Try the curve shapes, Invert, Reverse, Copy and Paste into Curve 2. Save the instrument as a preset and save/reopen the set → nodes, bends, segment modes, timing and routes persist.
8. Try the four LFOs, both steppers, eight macros and performance sources under More sources. Change Voice & mixer and Effects; move a knob from a mapped MIDI controller → the display follows.
9. Change track, undo a knob gesture, and change theme while the editor is open → its state is retained. Re-select Osmium → Expand reuses its embedded editor. Delete the instrument → its editor closes.
10. At a smaller window size, scroll to the controls. Check Dark and Light themes; verify Monolith still uses its original drawers and existing sets sound the same.

## 1. MIDI keyboard and controller (0.5)

1. Plug in a MIDI keyboard, select the Keys track and play → you hear it; the small ● next to KEY / MIDI in the control bar flickers.
2. Edit → Preferences → MIDI → your keyboard is listed and ticked; untick it → it stops playing; tick it again → it plays.
3. Unplug and plug the keyboard back in while Sessions runs → it plays again without restarting.
4. Hold the sustain pedal while you play → notes hold until you let go.
5. Arm a track (●), click the ● in an empty clip slot and play a few bars; click the clip again → it loops what you played.
6. Press Ctrl+M, click a track's volume fader, turn a knob on your controller → it's mapped (blue label); Esc, then turn the knob → the fader follows.
7. In MIDI Map mode, click a clip slot and hit a pad → the pad launches that clip.
8. Ctrl+K (Key Map): click a scene, press a letter key → that key launches the scene.
9. Save, close, reopen the set → the mappings still work.

## 2. Audio output (0.5)

1. Preferences → Audio: pick your sound card, try 10 ms and 50 ms buffers → playing keys feels tight at 10 ms; no crackles at 30 ms while the demo plays.
2. Play a few notes at 30 ms → you shouldn't feel a delay between key and sound.

## 3. Devices — listen to them (0.6)

Select a MIDI track, Shift+Tab to the Device View, double-click devices in the browser.

1. **Analog Synth**: change Osc 1 / Osc 2 waves, drag the filter dot, drag the envelope handles → each change is audible, no clicks while turning knobs.
2. **FM Synth**: click the algorithm diagram to step through the 7 algorithms, raise C / D Level → the tone changes from soft to bell / metallic.
3. **Wavetable Synth**: drag across the wavetable display (Position) while holding a chord; try each Table and Unison 1 / 3 / 5 → smooth morphing, wider with 5.
4. **Sampler**: drop a sample on it, drag the start / end flags, turn Loop on and hold a key → it loops between the flags.
5. **Organ**: pull drawbars, switch Percussion on, try Vibrato C3, Rotary Slow → Fast → the horn speeds up over a second or so and the sound swirls.
6. **Bass**: play overlapping notes (legato) → they slide; play hard (velocity > 100) → accented, brighter. Then Browser → **Sounds** → **Acid Line** (a ready-made Bass setting) → squelchy 303-style line.
7. **Electric**: play softly and hard → bark on hard notes; the tremolo lamps pulse only while notes sound (fixed in 0.10: retest).
8. **Drum kits**: click the pads → each sound plays and the pad lights.
9. **EQ**: drag the band dots on a playing track → you hear the bands move; mouse wheel on the mid band changes its width.
10. **Compressor** on drums: lower the threshold → the orange GR meter moves and the drums get squashed.
11. **Saturator**: try each Type → Analog warm, Hard Clip harsh, Fold strange, Digital gritty.
12. **Auto Filter**: LFO Amount up → the filter sweeps in time with the song; try Shape and Rate.
13. **Delay** (Ping Pong on) and **Reverb** (Freeze on, then off) → echoes bounce left / right; Freeze holds the tail.
14. **Chorus, Phaser / Flanger, Auto Pan, Redux, Gate, Limiter, Utility**: one quick listen each → each does what its name says, no pops when switched on / off (every device now fades over a few ms: retest).
15. **Arpeggiator, Chord, Scale, Velocity** in front of a synth: hold a chord → arpeggiated / extra notes / no wrong notes / evened-out velocity.
16. Browser → **Sounds**: try Glass Keys, Warm Pad, Rock Organ, Tubular Bells, Formant Choir, Reese Bass → all sound usable, none much louder than the others.
17. Turn any knob, press Ctrl+Z → it goes back (this didn't work before 0.6).

## 4. Recording and mixing (0.7)

1. Preferences → Audio → Input device: pick your microphone / interface → the status line shows it.
2. Make an audio track (Ctrl+Shift+T), arm it, Monitor *Auto* → you hear yourself; the meter shows your input.
3. Record into a slot (● in an empty slot), clap or play along with the metronome on → the recording loops; **check the claps line up with the beat** (if they're early or late, tell me by how much: zoom in on the Arrangement).
4. Arrangement Record (● in the control bar) with an armed audio track → the take appears on the timeline where you played it.
5. Ctrl+click two track titles, Ctrl+G → a group track; its fader moves both; fold it with ▾; Ctrl+Shift+G ungroups.
6. Clip View → Envelopes: pick Mixer → Volume, Show, click a few points → the clip's volume follows them while it plays, looping.
7. Drop a long WAV / MP3 on an audio track → the window doesn't freeze while it loads.
8. Clip View → Sample: Warp off → the clip plays at its own speed whatever the tempo; Slices 1/16 vs Transients → slightly different feel on drums.

## 5. Drums and performance (0.8)

1. Browser → Drums → **Drum Rack**: click pads (808 sounds), drag a sample from your files onto a pad → it plays the sample; right-click a pad → cleared.
2. Program a hat pattern with open and closed hats → the closed hat cuts the open one (choke).
3. **Macros** after a synth: right-click a macro → Map to → a filter cutoff; turn the macro → the cutoff knob turns (green value).
4. Right-click the metronome (◉) → Count-in 1 bar; Arrangement Record + Play → one bar of clicks (position shows -1 in red), then recording starts.
5. Turn on ⇥ and ⇤ (punch in / out) around the loop brace, record over a longer stretch → only the part inside the brace is recorded.

## 6. Racks (0.9)

1. Browser → Sounds → **Layered Keys** on a MIDI track → a piano with a pad underneath; Macro "Pad Vol" fades the pad in and out; "Cutoff" opens it.
2. In the rack's chain list click **Pad** → its devices (Warm Pad, Chorus) appear next to the rack; click **Keys** → the Electric piano.
3. Chain controls: switch a chain off (the small square), solo one (S), drag Volume and Pan → you hear each change, no clicks.
4. Drag another instrument from the browser onto the chain list (not onto a row) → a new chain plays it alongside the others.
5. Drop an effect onto the drop zone next to a chain's devices → it goes into that chain only.
6. **Audio Effect Rack**: Sounds → **Parallel Crush** on the drums → the dry drums plus a crushed copy; Macro 1 sets how much. Try **Wide Space** on a pad.
7. Right-click a Saturator's title bar → Group into Audio Effect Rack → it's now in a rack, sounding the same; right-click the rack's title → Ungroup Rack → back as it was.
8. Right-click a macro on a rack → Map to → a chain → a device knob → turning the macro turns it.
9. MIDI Map (Ctrl+M) a chain's volume to a controller knob → the knob fades that chain.
10. Save, close, reopen → the racks, their chains and devices, and the macro mappings are all back.
11. Ctrl+Z after each of the steps above → each comes undone in one step.

## 7. Your interface and playing through the app (0.10)

1. One-time interface setup: in a terminal run `wpctl status`, find **UMC202HD 192k** under Audio → Devices and note its number, then `wpctl set-profile <that number> 1` (Analog Stereo Duplex) → `wpctl status` lists a "UMC202HD 192k Analog Stereo" source (the current profile calls the input "Digital (IEC958)", which is wrong for this interface).
2. Edit → Preferences → Audio: Audio system **PipeWire / JACK**, Buffer **128 samples**, Main out L / R = the UMC's playback FL / FR, Input 1 = the UMC's capture FL (your guitar's input), Input 2 = capture FR → the line underneath says about 10 ms from input to output, with no warning.
3. Create → **Insert Guitar Track**, then play → you hear the guitar through the amp with no delay you can feel; the track's meter moves.
4. Buffer **64 samples** → even tighter; play hard for a minute → no crackles (if it crackles, 128 is the setting to keep).
5. **Tuner**: tune all six strings → the note and needle are steady, green when in tune; try Strobe; Mute silences the track while you tune.
6. **Guitar Amp**: step through the five heads; sweep Gain, Bass, Mid, Treble, Presence, Volume → each head sounds clearly different, the curve and the meters move.
7. Cabinets and mics: try each cab and On Axis / Off Axis / Far → from bright and tight to dark and roomy; Cab Off → harsh, as without a speaker.
8. **Load IR**: if you have a cabinet impulse response (a WAV), load it with Load IR… or drop it on the amp → the cabinet shows its waveform and name, and it sounds like that cab.
9. **Pedals** in front of the amp, one at a time: Overdrive, Distortion, Metal Distortion, Chainsaw, Metal Boost, Fuzz, Crunch, Twang, Phase Pedal, Chorus Pedal → each sounds like its kind of pedal; the footswitch and light switch it on and off without a pop.
10. **Wah**: drag the rocker while you play → the classic wah sweep; Ctrl+M, click the rocker, move an expression pedal or a knob → it rocks; Mode Auto → your picking opens it.
11. Browser → **Guitar** → the rigs: **Metal Rig**, **Swedish Chainsaw**, **Blues Crunch**, **Clean Twang** (and **Bass Amp** with a bass) → each is ready to play; their macros turn the main knobs.
12. Arm the Guitar track and record a clip (● in a slot) along with the metronome → the recording plays back in time with the beat.

## 8. Dragging devices around (0.11)

1. Drag **Reverb** from the browser onto another track's column in the Session View (its title, a slot, anywhere in the column) → the column gets an orange outline while you hover; on release the Reverb is on that track and its Device View opens.
2. Drag **Chorus** onto a track's mixer strip, and **Gate** onto a track's header in the Arrangement → each lands on that track.
3. Drag a Sound (say **Acid Line**) onto the empty space right of the last track (in the Arrangement: onto the Main lane or below) → a new MIDI track named "Acid Line" plays it; Ctrl+Z removes the track in one step. An effect dropped there makes an audio track.
4. Try dropping an instrument onto an audio track → the pointer shows it can't go there and nothing changes.
5. In the Device View, drag **EQ** from the browser between two devices → an orange line shows where it'll go; it lands exactly there. Drop an effect in front of the instrument → it goes right after the instrument instead.
6. Drag a device by its **title bar** to another place in the chain → it moves; hold **Ctrl** while dropping → a copy goes there and the original stays.
7. Drag a device's title onto another track's Session column (or its Arrangement lane) → it moves to that track with its settings (and its automation, if it had some). Ctrl → a copy.
8. Drag a long chain's device towards the Device View's left or right edge → the chain scrolls.
9. Click a device's title → it's highlighted; Ctrl+click another, Shift+click a third → several selected. Then try Delete, Ctrl+D (copies right after), Ctrl+C, select another track, Ctrl+V (pasted there).
10. Select an instrument and an effect, press **Ctrl+G** → both are in one chain of a new Instrument Rack, sounding the same; select the rack, **Ctrl+Shift+G** → back as they were.
11. Drag a device into a rack (next to the rack's devices, or onto its chain list) and back out onto the track → it moves in and out; a macro mapped to it lets go when it leaves the rack.
12. Double-click a device's title → it folds to a slim strip with its name on end; double-click again → it's back. Save and reopen → folded devices stay folded.
13. Right-click a device's title (or several selected, or a rack) → **Save as Preset…**, name it → Browser → Places → **User Library** lists it. Drag it onto another track, or double-click it → the same devices and settings. Right-click it there → Rename…, Delete, Show in File Manager.
14. In the Session View, drag a scene by its name (not its ▶) to another row → the whole row of clips moves; a plain click on the name still launches the scene.
15. Ctrl+Z after each of the steps above → each comes undone in one step.

## 9. Takes, comping and sidechains (0.12)

Since 0.12 an audio track that monitors its input (Monitor *In*, or *Auto* while armed) plays its input **instead of** its clips. So after recording a clip, disarm the track (or set Monitor Off) to hear it; 4.3 above now works that way.

1. On your guitar track (armed, Monitor Auto), turn the loop on around 4 bars, Arrangement Record ●, Play, and play the part three times → the first pass makes a clip over the loop; while the second and third pass record you hear only yourself, not the pass before; Stop → the clip's Sample box says **3 takes**.
2. Disarm the track and press Play → you hear the last pass (the newest take).
3. Select the clip, **Sample → Takes (3)** → the Takes view: a Comp lane on top in the track's colours, Take 3, Take 2, Take 1 under it. Drag across Take 1 from bar 1 to bar 2 → the Comp lane shows that stretch in Take 1's shade; Play → bar 1 is now your first pass, the rest the third, and the join is clean (no click, no gap).
4. Click a lane (one grid step), double-click a lane (the whole take), right-click → Newest Take Everywhere → each does what it says; Ctrl+Z after each → one step back each time.
5. Record four passes but stop in the middle of the fourth → the fourth is kept as a take (short), and the clip plays the fourth where you played and the third where you'd stopped.
6. Save, close, reopen → the takes and the comp are still there and play the same; File → Collect All and Save copies every take's file into the set's folder.
7. On a bass track (a Sampler or Bass instrument playing a long note, or your guitar with Monitor In), add a **Compressor**; in its Sidechain column choose **Key From → Drums** (a track with a kick pattern), Threshold around −24 dB, Ratio 6, short Attack, Release 150 ms → the bass ducks on every kick; the GR meter and the Key meter move with the kicks; mute the Drums → with **Key Tap Post FX** it still ducks (Post Mix stops ducking); unmute.
8. Turn **Listen** on → you hear the drums through the bass track instead of the bass; off → the ducked bass. Export Audio with Listen on → the file has the ducked bass, not the drums.
9. **Gate** on a pad keyed from the drums → the pad stutters with the kick; **Auto Filter** keyed from the drums with Env up → the filter opens on each hit.
10. Try to key the Drums' own Gate from the bass track (which is keyed from the drums) → the menu greys it out; delete the Drums track → the Compressor's Key From says Missing in red and nothing ducks; Ctrl+Z → it ducks again.
11. Save, close, reopen → the key source, tap and knobs are back. Save the Compressor as a preset and drop it on another set's track → its knobs are kept, Key From is Internal again.

## 10. Monolith, the big synth (0.13)

1. Browser → Sounds → **Growl Bass** onto a MIDI track; play C1–C2 on your keyboard → a growling bass that moves on its own (LFO 1 sweeps the wavetable); hold a note and turn **Macro 1** (Mod page) → the filter opens; **Macro 2** → more bite. Try **Wobble Sub**, **Reese Monolith**, **Neuro Step** (the steppers run with the song: press Play), **Formant Talk**, **808 Sub**, **Sync Bass** → each sounds like its name, none much louder than the others, no clicks between notes.
2. Browser → Instruments → **Monolith** (the plain one): Osc page: turn Osc 1's **Pos** while holding a note → the sound morphs; choose another **Table** for each oscillator; **Bend**, **Formant**, **Sync** with **Intensity** up → three different kinds of growl; **Pitch** −12 on Osc 2 with the **Sub** table → a sub under it.
3. **Mod Osc**: Phase up → FM-like bite; Ring up with Pitch +7 → metallic; **Noise** with Color down → low rumble; **Feedback** up → thicker, still stable at full.
4. Filter page: F1 **Scream** with Reso high, Cutoff low → it screams; **Comb** → ringing; drag the dot in the response; **Routing** towards series and **F Mix** towards F2 → F2 shapes F1's output; **Insert** Tanh / Fold / Bits with Amount → dirt, no silence or runaway.
5. LFO page: drag bars in **Step 1** to draw a pattern, set Steps to 8; Mod page: drag the **Step 1** chip onto F1 Cutoff; press Play → the cutoff follows your pattern in time with the song; Glide up → it slides between steps.
6. Drag **LFO 1** onto Osc 1's Pos, set the amount to −50 % in the list → it sweeps the other way; **✕** → gone; Ctrl+Z → back. Right-click a knob → Modulate by → Env 2 → same thing without dragging.
7. Voice page: **Legato** with Glide ~100 ms, play overlapping notes → they slide, no retrigger; **Mono** → each note restarts the envelope; **Poly** → chords; Unison 4 with Spread and Detune → wide and thick.
8. MIDI Map (Ctrl+M) a Macro knob and a step bar's knob isn't visible, but map **Macro 1** to a controller knob and Osc 1 **Pos** to another → both move; draw automation for F1 Cutoff in the Arrangement → it plays back.
9. Save, close, reopen → the routes, pages' knobs and the sequencers' patterns are back; right-click the title → Save as Preset → the preset in User Library brings the routes with it.
10. Ctrl+Z after each step above → one step back each time.

## 11. Themes, and Session clips into the Arrangement (0.14)

1. **View → Theme → Light** → the whole window changes at once, no restart: menus, browser, Session grid and mixer, Clip View (piano roll: light grid, keys still white and black), Arrangement (light lanes, dark grid lines, dark playhead and selection outlines), Device View (light panels, light displays with orange and teal curves, dark knob pointers). Nothing stays dark except meter troughs and the guitar pedals' / amp's faceplates; no text is unreadable; numbers in value fields are dark, not orange. Hover a few controls → the Info View and tooltips are light too.
2. Open an EQ, Compressor (with Sidechain), Drum Rack, Instrument Rack, Monolith (all six pages) and the Guitar Amp in Light → every display and label readable; a Monolith modulated knob's value still shows in teal.
3. **View → Theme → Mid** → a mid-grey scheme in between, dark text. **Dark** → exactly as it always looked (compare with 0.13 if you have it side by side).
4. Choose Light, quit, start Sessions again → it starts light. View → Zoom → restart → still light. Light stays while you open and save sets.
5. In Light: drag a device from the browser over a track → the orange drop outline and the Device View's orange line are visible; Ctrl+M → the map tints are visible; Esc.
6. **Session → Arrangement:** in the Session View, press on the Drums' *Beat A* (not on its ▶), move a little, and press **Tab** while holding the button → the Arrangement appears with a faded *Beat A* under the pointer and an orange line at the beat it would start on; move along the Drums lane → it snaps to the grid; release → a copy appears there, selected, and the Clip View shows it. The Session slot still has *Beat A*; press Space → both play (the Arrangement one when the Session's isn't launched). Ctrl+Z → the copy is gone.
7. Same again holding **Alt** when you release → it lands exactly under the pointer, off the grid. Release over the *Bass* lane with a MIDI clip from Drums → fine (both MIDI); over an audio track → forbidden cursor and nothing lands; over a return lane, the headers or the ruler → nothing.
8. Drop a clip onto an existing Arrangement clip → the old one is cut around the new one (like Paste); one Ctrl+Z restores both.
9. Pick up a clip, Tab, then **Tab again** while still holding → back in the Session View with the clip still in hand; drop it on another slot → it moves there as a normal slot drag. Pick up, Tab, **Esc** → the clip is let go, nothing lands, the Arrangement stays.
10. Do 6 while the song plays → the drag and drop work the same, playback never stops.

## 12. NAM: Neural Amp Modeler captures (0.15)

Get a few captures: [tonehunt.org](https://tonehunt.org) (free `.nam` files; "standard" and "lite" WaveNets, some LSTMs), ideally one amp-only capture and one full rig (amp + cabinet). Put them in a folder and add it to the browser (Add Folder…), or drop them into `~/.local/share/Sessions/Sessions/Presets` to see them in the User Library.

1. Plug in the guitar, Create → Insert Guitar Track, then drag a `.nam` from the browser onto the track → a **NAM** device appears after the amp, its panel shows the capture's name, network ("WaveNet · 16/8 channels · 2 × 10 layers" for a standard one) and gear; play → you hear the capture (switch the Guitar Amp off to hear it alone). Delete the amp.
2. Drop a different `.nam` straight onto the NAM panel → it replaces the capture, no click or dropout; Ctrl+Z → the old one is back.
3. Turn **Input** up and down while playing → more and less drive, like the guitar's volume; **Output** only changes the level. The In / Out meter follows.
4. An amp-only capture sounds fizzy: drop a cabinet IR (a WAV) on the **Cabinet IR** box → the fizz is gone; the **IR** button A/Bs it, IR Mix blends. A full-rig capture: leave the IR off.
5. Stop playing with the guitar's volume up → hiss / hum; turn **Gate** up until it closes between notes and opens on the first note without eating the attack (all the way down = off).
6. Two captures with very different levels: **Normalize** on both → they sit at about the same loudness (it reads the loudness in the file; the panel says "no loudness data" when a capture has none).
7. CPU: with a standard capture on a mono guitar track at 128 samples, the audio stays clean (no crackle); add a second NAM → still clean. (A standard capture is about a tenth of a core; a stereo clip through one uses two networks.)
8. Double-click a capture in the browser with the guitar track selected → a NAM device holding it is added; drop one on the empty space under the tracks → a new audio track named after the capture.
9. Save the set, quit, move the folder (with the captures copied next to it, or File → Collect All and Save first), reopen → the captures load. Rename a capture file away → the panel says "Missing: …" in red and the track plays dry; put it back and reload → it's back.
10. Right-click the NAM title → Save as Preset → the preset in User Library brings the capture (and the IR) with it.
11. A capture you downloaded that won't load (a very new "A2" file, or a stereo one) → the panel shows the reason in the Load dialog's warning and the device plays dry; nothing crashes.
12. View → Theme → Light with the NAM panel open → the capture view, meter and IR box are readable; "Missing" is still red.
13. A new-style capture (the panel's summary ends in "2 sizes", like your Jcm800.nam): switch **Quality** to Light while playing → the sound stays close, the CPU drops; back to Full. A single-size capture says "one size" and ignores the switch.

## 13. Racks, completed: pads with devices, zones, the MIDI Effect Rack (0.16)

1. On a MIDI track with a **Drum Rack**, drag **Analog Synth** (or a bass Sound, or Monolith) from the browser onto the **E1** pad → the pad shows the synth's name in blue and the synth's panel appears next to the Drum Rack; program a few E1 notes → the synth plays them (at C3) instead of the tom, the other pads keep their samples.
2. With E1 selected, change its **Transpose** → the synth's note moves; **Volume** and **Pan** set its level and side. Drag a **Saturator** into the drop zone after the synth → only E1 gets saturated.
3. Drag **Redux** onto the **D1** (snare) pad → a dot appears on the pad, only the snare is crushed, and it stays in time with the kick (listen for flams). Click another pad → its devices (or an empty drop zone) show next to the Drum Rack.
4. Put a synth on **F#1** (closed hat) and hold a long F#1 note, then hit **G#1** (open hat, same choke group) → the synth releases at the hit.
5. Right-click a pad → **Remove Pad Devices** → the pad plays its sample again; Ctrl+Z → back. Right-click → **Clear Sample** leaves the devices alone.
6. Move the Drum Rack to another track by its title, Ctrl+D it, save it as a preset, save and reopen the set → the pads' devices come along every time. Try to drag the Drum Rack into an Instrument Rack's chain → refused while its pads hold devices.
7. Browser → Sounds → **Bass and Keys Split** → below G3 a sub bass, from G3 up an electric piano. Press **Zones** on the rack → the zone editor shows the two key ranges; drag the Bass bar's right end down to C3 → notes from C#3 now go to the keys.
8. In the zone editor, drag the upper half of a bar's end inward → a fade appears; notes in it play softer. Double-click a bar → back to the full range. **Vel**: give one chain 1 – 90 and the other 91 – 127 → play soft and hard: you hear one or the other.
9. **Chain**: give chain 1 the zone 0 – 0 and chain 2 the zone 1 – 1, then click on the scale above the bars (or turn the **Chain** field) → 0 plays chain 1, 1 plays chain 2. Map a macro to **Chain Selector** (right-click a macro) and switch sounds with it while playing; held notes ring on.
10. An **Audio Effect Rack** (Parallel Crush) with chain zones 0 – 10 and 0 – 20 with a fade: sweep the selector → the crushed chain fades in and out smoothly, no clicks.
11. Browser → MIDI Effects → **Sounds: Octave Layers** before a synth → every note also plays an octave up, softer; Macro 2 adds the octave below. **Split Arp** → notes below C3 arpeggiate, notes above play straight.
12. Select an Arpeggiator and a Chord (Ctrl+click their titles), Ctrl+G → a **MIDI Effect Rack** holding them, sounding the same. Add a chain, set its **Transpose** to +7 → fifths on top.
13. Drag a chain's name up or down in any rack → the chain moves, its devices, knobs and automation with it. Delete a chain with automation on a later chain's volume → that automation still moves the right chain.
14. View → Theme → Light → the zone editor, the Key / Vel / Chain buttons and the pads' blue names stay readable.
15. Ctrl+Z after each of the steps above → each comes undone in one step.

## 14. After the 0.16 hands-on: Session first, panes, clocks, Monolith, NAM (0.17)

1. Stop the song. Play live on a MIDI track, then click a drum clip's ▶ in the Session View → the clip starts, your playing goes on, and **nothing from the Arrangement plays** (the Session has taken every track; **Back to Arrangement** lights above the timeline). Click a scene's ▶ → the same. Click Back to Arrangement → the timeline plays from where it is.
2. Press Space (the Arrangement plays), then launch a Session clip → only that track switches to the clip; the other tracks keep playing the timeline (as before).
3. Drag the divider right of the browser → it gets wider (and narrower, down to about 220 px); quit and restart → the width is kept. View → Browser hides and shows it.
4. Open the Device View → the pane is exactly as high as the devices; try to drag the divider above it → it doesn't move. Open the Clip View → drag the divider up for a tall piano roll; switch to Device and back → the piano roll comes back as tall as you left it; quit and restart → still that height.
5. Launch a few clips and watch the clocks: the bar along the bottom of each playing clip and the small clock at its right fill once per loop in the clip's own colour, smoothly, without jumping; the stop row's clock (with the loop count and length) is in the track's colour. At the loop point they snap back to empty exactly as the clip restarts.
6. **Monolith**: Browser → Sounds → **Growl Bass** → the Synth page shows all three oscillators, the filters, the voice settings and the macros at once; drag the horizontal scroll bar if your screen is narrower than the device. The Mod page shows the envelopes, LFOs, steppers, sources and routes. Drag **Wheel** onto Filter 1's Cutoff, move your keyboard's mod wheel while playing → the filter opens.
7. Set **Bend Range** to 12 and bend the pitch wheel → the note slides an octave. With Bend Range 2 → a whole tone. Press harder on a keyboard with aftertouch after routing **Pressure** to Osc 1 Intensity → the growl comes in as you press.
8. **Restart** on: play the same bass note repeatedly → every hit sounds identical (tight attack); off → each hit a little different (free phases).
9. LFO 1 **Retrig** on, routed to the cutoff at 1/8 → every note starts the sweep from the same point; off → the sweep runs on through the song, notes catch it where it is. LFO **Fade** at 1 s with a sine LFO on Osc 1 Fine → vibrato that comes in a second after each note.
10. Step 1 **Retrig** on with a pattern routed to the level → every note plays the pattern from step 1; off → the pattern follows the song's beat.
11. Open a set saved with 0.16 that has a Monolith → it sounds the same (the new knobs start at their defaults: Bend Range 2, Restart off, Retrig off, Fade off).
12. **NAM**: put a few captures in the User Library folder and in a browser sample folder; load one → the panel says "n / N"; **▶** loads the next (in path order, folders included), **◀** the previous, and past the last it wraps to the first. A capture that isn't in those folders → "not in the library", and ◀ ▶ step through its own folder. Ctrl+Z after a step → the previous capture is back.
13. Ctrl+M (MIDI Map), click **▶** on the NAM, press a button or pedal on your controller, Esc → the pedal switches captures while you play (expect a short dropout while a big capture loads).
14. View → Theme → Light with the Monolith's two pages and a playing Session clip → readable; the clip clocks stay visible.

## 15. Everything in sync, and the pedals revisited (0.18)

1. Set the tempo to 172. Drop a drum loop you know is around 85–90 BPM onto an audio track → it plays in time with the metronome at half the set's tempo (an 87 BPM loop plays at 86); the Clip View's Sample box says "found 87.0 BPM · plays at 86.0 (1/2 of the set's tempo)" and the clip is a whole number of bars.
2. Drop loops at other tempos (a 128 BPM loop, a 100 BPM one) → each plays in time, doubled or halved as needed, and the Sample box says what it found and how it plays. Try a few of your own samples and tell me which were found wrong (and what they really are).
3. Drag the set's tempo from 172 down to 100 while the loop plays → the loop follows; crossing about 123 it re-matches (the 87 BPM loop goes from half the set's tempo to the set's tempo), and in the Arrangement its placement grows or shrinks to keep the same audio. Ctrl+Z undoes the tempo and the re-match together.
4. Drop a one-shot (a hit) and a vocal phrase that isn't a whole number of beats → the hit plays once at its own speed; the phrase plays once, in tempo. Press **Warp** off on a loop → it plays as recorded, whatever the tempo; on again → back in sync.
5. Move a clip's warp markers by hand, then press **Sync** → the clip is fitted to the set's tempo again from its audio. Open a set saved with 0.17 → its audio clips still play as before; press Sync on one → it's matched.
6. Drop a long file (a whole song) → it comes in warped to its found tempo (if it has one), playing once; the drop doesn't stall the window for more than a moment.
7. **Pedals**: Create → Insert Guitar Track, drop each pedal in front of the amp one at a time and play the same riff → each one sounds like a different box: Overdrive (clean lows, sweet mids), Distortion (hard, buzzy; try Clip Silicon / LED / Germanium), Metal Distortion (scooped high gain; sweep Mid Freq), Chainsaw (the buzzsaw with Low and High on 10), Metal Boost (tight, almost clean, lots of level), Fuzz (woolly, sustaining), Crunch (crunchy, touch-sensitive).
8. Each pedal's panel: knobs and buttons on the left, its clipping graph (what a quiet note turns into) and its tone graph on the right; turning Drive / Gain / Sustain changes the clipping graph, the tone knobs the tone graph. The title bar's activator switches the pedal (no footswitch).
9. Fuzz: play quietly with **Bias** on 10, then with Bias on 1 → sputtering, gated notes that cut off; loud notes still come through. Crunch: play soft and hard → the attacks bite harder; **Bright** on adds sparkle; **Presence** adds bite on top.
10. Overdrive **Asym** vs **808** on a single sustained note → Asym is a touch thicker (even harmonics). Distortion **Filter** up → darker; **LED** louder and more open than Silicon.
11. Phase Pedal: **Depth** down to 3 → a subtle swirl; **Script** on → smoother. The notch graph sweeps with the Speed. Chorus Pedal **Mix** at 100 % → vibrato; **Stereo** off → mono. Twang **Dwell** up → the springs drip; the tail rings after you stop.
12. Every pedal at its defaults sits at about the same loudness as the dry guitar (within a few dB); none clips the track meter at a normal playing level.
13. View → Theme → Light → the pedal graphs, the Sample box's tempo line and the Sync button are readable.

## 16. Themes, the compact Clip View, the overview bar, impulse responses, the Drum Studio (0.19)

1. View → Theme → **Midnight** → the whole window goes blue-black with an amber accent, at once; **Ember**, **Forest**, **Plum**, **Slate**, **Paper**, **Sky**, **High Contrast** each look as named; back to **Dark** → exactly as before. Quit and restart → the theme is kept.
2. View → Theme → **Customize…** → Start from **Forest**, drag **Hue** → the greys change colour live; drag **Contrast** → the text and panels pull apart; click the **Accent** swatch and pick a red → lit buttons, the selected tab and the device curves go red; text on them stays readable (dark or light by itself).
3. In the editor, click a swatch (say `slotEmpty`) and pick any colour → only that changes; press its **↺** → back. Give it a name, **Save** → it's in View → Theme, checked; **Delete** → gone from the menu.
4. The Device View with a few panels open under **Paper** and **Sky** → knob labels and values are readable (dark on light), displays and curves too.
5. The Clip View: the boxes are narrow with small fields; everything still reads. Press the round **L**, **♪** and **E** switches at the bottom left → the Launch, Notes / Sample and Envelopes boxes hide and show; quit and restart → the choice is kept.
6. The strip along the bottom of the window: select clips and tracks → the left says what's selected, the clip overview shows the clip's notes (or waveform) in the track's colour with the playhead running while it plays, the chain overview shows the track's devices as blocks (a switched-off device dim). Click the clip overview → the Clip View; click a block → the Device View with that device selected.
7. Browser → **Impulse Responses** → fifteen built-in cabinets (five cabinets × On Axis / Off Axis / Far). Drag **4x12 Closed · On Axis** onto a guitar track that has a **Guitar Amp** → the amp's cabinet switches to Loaded IR with it; onto a **NAM** → its IR slot, switched on; onto a track with neither → a **Cabinet** device appears holding it; double-click one → the same on the selected track.
8. Put a few WAV IRs in the folder (right-click the category → Show Impulse Responses Folder) and add a folder of your own with **＋ Add IR Folder…** → they're listed after the built-ins; the Cabinet's **◀ ▶** step through all of them in order and wrap; the panel says "n / N". Ctrl+M, click ▶, press a pedal → the pedal steps IRs.
9. A **Cabinet** on a DI bass or a NAM-without-speaker track: **IR** off / on to compare, **Mix** halfway, **Low Cut** up → the mud goes, **High Cut** down → darker. Save and reopen the set → the IR (a built-in one too) comes back.
10. Browser → Drums → **Drum Studio** on a MIDI track; play C1, D1, F#1, A#1, the toms and cymbals on a keyboard → a full kit; hit the snare soft and hard → ghost notes and rimshot-like cracks; the kit view lights the drum. Switch **Kit** through all six → each a different kit (Vintage dull and low, Tight Pop short, Metal clicky and bright, Jazz open, Big Room boomy).
11. Click a drum in the kit view → it sounds and its knobs show; **Tune** the toms, **Decay** the kick long, **Tone** the hats dark; **Wires** down → a dry snare, **Beater** up → more click.
12. **Mics** page: **Overheads** off → the kit goes flat and close; back on with **Width** wide → the hats left, the ride right; **Room** up with **Room Size** large → a big room tail; **Bleed** up → glue. **Velocity** at 0 → every hit the same; at 100 % → wide dynamics.
13. **Grooves** page: pick Funk → Ghost Notes, press **To Clip** → a clip in the track's next free slot, playing a funk beat; drag **Jazz → Swing Ride** onto a Session slot → a swung ride pattern; drag **Metal → Blast Beat** onto an audio track's slot → a new "Drums" MIDI track with a Drum Studio and the clip; drag **House → Four to the Floor** onto an Arrangement lane → a clip at that beat. **Humanize** Vel and Time up before dragging → the hits vary a little.
14. Drop a groove's clip onto a track with the **808 Kit** or a **Drum Rack** → it plays there too (the same notes).
15. Ctrl+Z after each of the steps above → each comes undone in one step.

## 17. The Monolith reworked (0.20)

1. Browser → Sounds → **Growl Bass** → the Device View shows the main panel: three oscillators with big Position and Level knobs, the Sub column, both filters with big Cutoffs, Master with the Volume and macros; the six drawer buttons (Env, LFO, Step, Mod, Voice, FX) to its right.
2. Click **Env** → the envelope drawer slides out to the right (the devices after it move over); hold a note → a dot rides Env 1's curve through the attack, decay, sustain and release as you hear it; drag the decay handle while holding the note → the sound follows at once. Click **Env** again → it slides back; click **FX** then **Mod** → one drawer at a time.
3. Open the Clip View and come back → the drawer you had open is open again. Save the set and reopen → the sound is the same (the drawer state isn't saved, the knobs are).
4. **Sub** up on Growl Bass → an octave of weight under the note; **-2** → two octaves; **Square** → it growls. **Drive** (filter 1) up → thicker, with harmonics. Voice drawer → **Mod Osc Wave** Square with Ring up → a harsher ring modulation than Sine.
5. LFO drawer → LFO 1 **One-shot**, shape Saw Down, free 2 Hz, routed to Filter 1 Cutoff (drag its chip onto the knob) → each note opens the filter and closes it once over half a second, then holds; **Loop** → it keeps wobbling.
6. FX drawer → **Delay Mix** up with 1/4 → repeats in time with the song (change the tempo → they follow); **Feedback** up → more of them; **Tone** down → darker repeats. **Reverb Mix** up with a big **Size** → a tail; **Chorus Mix** up → wide; **EQ Low** up → more bottom. Route **Macro 1** to Delay Mix (right-click the knob → Modulate by → Macro 1) → the macro brings the delay in.
7. Sounds → **Dub Wobble** (the mod wheel moves the growl, Macro 1 the cutoff), **Neuro Reese**, **Talking Growl** (press harder with aftertouch), **Supersaw Lead** (the wheel spreads the detune, Macro 2 the delay), **Glass Pad** (hold a chord), **Acid Monolith** (play legato runs), **Hoover** (every note drops in pitch), **Stab**, **Deep Sub** → each as named, none clipping the track meter at a normal level.
8. Open a set saved with 0.19 that has a Monolith → it sounds the same (the new knobs start at their defaults: Sub off, Drive off, Loop, FX off).
9. View → Theme → **Paper** with the Monolith open → the main panel, the drawer buttons and every drawer readable.
10. Ctrl+Z after each knob move and route → each comes undone in one step.

## 18. After the 0.20 hands-on (0.21)

1. Browser → **Audio Effects** → no Tuner, amp, NAM, pedals or Cabinet; Browser → **Guitar** → all of them, then the rigs; **Sounds** → no rigs.
2. Arrangement, the overview strip at the top: press and drag **sideways** → the timeline scrolls with the outline under the pointer; drag **down** → it zooms in around the beat you pressed; drag **up** → out; click → the view jumps there. Preferences → Look → **Zoom drag: Drag up to zoom in** → dragging up now zooms in, in the overview, the beat rulers and the clip overviews alike.
3. Select a clip. The strip along the bottom shows it with an outline around what the editor shows; drag it sideways → the piano roll / waveform scrolls; drag down → it zooms in. The strip over the Clip View (between the Arrangement's time ruler and the editor's ruler) does the same; click it → the editor centres there.
4. The bottom strip's device chain shows little pictures of the actual device panels (a Monolith's is wide); with a long chain, drag it sideways → it scrolls; click a picture → the Device View opens with that device selected.
5. Arrangement: drag across the **body** of an audio clip → a part of it is selected (the clip stays put, its name bar still moves it). **Ctrl+C**, click an empty place later on the lane, **Ctrl+V** → a new clip of that part appears; drag its right edge out → the rest of the sample is there (nothing was cut away). **Ctrl+X** on a selected part → the clip is cut around it, one Ctrl+Z puts it back. Delete → the part is cleared.
6. **Ctrl+drag** a clip by its name bar → a copy lands where you drop it, the original stays; Ctrl+drag a selected part → a copy of the part; drag a selected part without Ctrl → it splits out and moves.
7. The **Clip** / **Device** tabs stand on their side between the Info View and the detail pane; they switch as before, and Shift+Tab too.
8. Arrangement Record ● on, Play, then **Stop** → the record button goes out (press Play again → nothing is recorded until you turn it on).
9. An audio clip's Sample box → **Warp Mode** lists Beats, Tones, Texture, Re-Pitch, Complex, Complex Pro. Halve the set's tempo and compare on a drum loop (Beats stays crisp), a bass line or a vocal (Tones stays at pitch and smooth), a pad (Texture: cloudy, more with Flux up), a full mix (Complex keeps every instrument's pitch; Complex Pro keeps chords cleaner and hits sharper). Transpose a vocal −12 in Complex Pro with **Formants** at 100 % → it still sounds like a voice, not a giant. Re-Pitch → the pitch follows the tempo.
10. The piano roll, the Arrangement lanes and the sample editor shade every second beat a little lighter; zoom far out → every second bar instead.
11. Ctrl+Z after each of the edits above → each comes undone in one step. Save and reopen → the warp mode and its grain / flux / formants come back.
12. **Second pass:** the strip over the Clip View starts where the editor starts (right of the boxes), not at the Info View. In the sample editor, drag the strip hard to the left and zoom out with Ctrl+wheel → the view stops at 1.1.1 and at the whole sample; Ctrl+wheel over a hit → that hit stays under the pointer.
13. Sample editor: hover the strip under the ruler → grey ticks at the hits light up; drag one → it becomes a yellow marker and the audio stretches; Shift+drag the waveform → the whole sample slides under the grid (snapped; Alt free); drag the waveform → it scrolls. Right-click → **Set 1.1.1 Here**, **Warp From Here**, **(Start at 1.1.1)**, **(Straight)**, **Warp 120 BPM From Here**, **Reset Warp Markers** → each does what it says, each one Ctrl+Z.
14. The beat shading is faint; zoom a 128-bar clip to fit → it alternates per bar or per four bars, not per beat.
15. Drag **Arpeggiator** (MIDI Effects) onto the free space right of the tracks → a new MIDI track with it; onto an empty audio track's column → the track turns into a MIDI track and takes it (its header colour band stays); onto an audio track with clips → refused with a message. Drop a WAV on an empty MIDI track → it becomes an audio track with the clip. Ctrl+Z → each change of kind comes back.
16. The play triangles in the slots, scenes and the transport, the stop square, the record discs (transport, arm buttons) and the browser's Preview are crisp at any theme; a checked (lit) button keeps a dark glyph.
17. **Third pass, live looping:** stop the song, set the launch quantization to 1 Bar, click a scene's ▶ at the right → every clip in the row starts at the same instant (none flashes for a bar first); while the song plays, click another scene's ▶ → all of its clips switch together on the next bar. Launch a single clip while playing → it waits for the bar as before.
18. **Master:** the rightmost column reads **Master** (the Arrangement's bottom lane too). Click its header → the Device View shows an empty chain with "Drop Audio Effects Here"; drop a **Compressor** or **Limiter** there (or on the column / lane) → it audibly acts on the whole mix; drag its Threshold down → everything ducks. Drop an **Analog Synth** on the Master column → a new MIDI track instead. Save, reopen → the Master's devices are back; Ctrl+Z after adding → gone.
19. **Colours:** right-click a track title → **Color** → a grid of squares (14 across, vivid on top, greys at the bottom); click one → the track recolours at once. Right-click a clip (slot or timeline) → **Color** → pick a vivid one → that clip alone changes, in the slot, the timeline, the Clip box name, the piano roll / waveform ruler and the overviews; **Track Color** gives it back. Ctrl+Z each step.
20. **Waveform:** in the sample editor the waveform is light grey-blue in Dark (dark in Light), whatever the clip's colour; the loop bar in the ruler keeps the clip colour. Click in the waveform of a timeline clip → the start marker jumps there (Space plays from it; while playing, the playhead jumps). Click in the waveform of a Session-only clip → the clip launches from that beat at once.
22. **Curves (fourth pass):** Browser → Sounds → Growl Bass, open the **Curve** drawer. Right-click Curve 1 → **Random 8 Steps**, drag its chip onto Filter 1's **Cutoff**, hold a note → the cutoff steps through a phrase over a bar, in time; drag a point up or down → the phrase changes while you listen; Alt+drag → off the grid; double-click a point → gone; **Retrig** on → every note starts the phrase from its beginning; **One-shot** → it plays once and holds; **Smooth** up → the steps become slopes; Rate → 2 bars → twice as slow. Save, reopen → the curve is there. Ctrl+Z after each step.
23. **Clips from the left and no overlaps:** launch a Session clip mid-song with Arrangement Record on, stop, then drag the recorded clip's **left edge** back to the song's start → it extends back and keeps showing the same audio where it was (a loop wraps). Drag a clip's name bar onto another clip → the other is cut away underneath, nothing overlaps; lengthen a clip's right edge over the next → the next one's start is cut. Ctrl+Z restores.
24. **The strip:** select a green clip, then a blue one → the strip over the editor shows the blue clip at once (no stale waveform).
21. **Monolith after Massive:** LFO drawer → four LFOs; set LFO 3 to Square, free 4 Hz, drag its chip onto Osc 1's Level → the sound stutters; the Mod drawer lists LFO 3 / 4 and Macro 5 .. 8 among the chips; the Master section shows eight macros. Env drawer → turn Env 1's **Curve** right → the attack creeps in then jumps; left → the reverse; **Loop** on with a short decay and sustain 0 → the note pulses while held. Sounds from 0.20 open unchanged (the new knobs at their defaults).

## 19. After the 0.21 hands-on (0.22)

1. Arrangement Record ● on, Play, launch a Session audio clip and a MIDI clip → each take shows on the timeline at once as a solid clip in the track's colour with a red frame, the audio one with its waveform, the MIDI one with its notes, growing smoothly with the playhead. Arm an audio track and sing → its take draws the input's waveform as it comes in, no gaps or jumps.
2. Stop → the tracks the Session took over are dimmed; click **Back to Arrangement** → the clips come back solid right away (no click in the view needed).
3. Record into an empty Session slot of an armed audio track → the Clip View's sample editor shows "● Recording" and the waveform as it comes in.
4. Select an audio clip. In the sample editor, above the waveform: the ruler, the loop brace in the clip's colour, the ▶ start and ◀ end triangles, the warp strip. Drag the ▶ a beat to the right → the triangle follows the pointer, the audio stays put, the ruler's "1" moves; play it → the clip starts a beat later into the audio and ends where it did. Drag the ◀ → the length changes. Drag the brace's body → the loop slides along the audio, same length. Ctrl+Z each step.
5. Drag across the waveform → a highlighted stretch; press **Delete** → it's gone from the audio and what followed moved earlier (the file name in the editor says "(edit)"); Ctrl+Z → the original file is back. Right-click a selection → **Crop to Selection** → only it is left, from 1.1.1; **Loop Selection** (Ctrl+L) → it's the loop. A click (no drag) still plays from there; the middle button drags the view.
6. In the Arrangement, drag across the body of a clip → Delete → that part is cleared (as before).
7. Right-click a slot clip → **Save Clip to User Library…** → name it → Browser → User Library → **Clips** lists it. Drag it onto an empty slot → a copy there; onto a MIDI lane (an audio clip) → a new audio track at that beat; double-click it → it goes on the selected track. Right-click it in the browser → Rename / Delete. The Clip box has **Save to Library** too, and the piano roll's and sample editor's menus.
8. Right-click around: a timeline clip (Cut / Copy / Paste, Rename in place, Loop, Reverse, Warp, Save…), an empty lane (Paste at the click, Insert MIDI Clip), a slot (Launch / Stop, …), a stop button (Stop Track / Stop All), the Master header (Back to Arrangement), a track title or a mixer strip (Track Off, Solo, Arm, Monitor), inside the piano roll (notes, octaves, grid), the envelope editor (breakpoints, Clear Envelope) → each menu does what it says.
9. Browser → a sample folder with subfolders → folders first with arrows, closed; open one, click another category and back → it's still open. Type in Search → a flat list of matches under their sources.

## 20. A real start marker and loop brace (0.23)

1. Select an audio clip (a drum loop). In the sample editor, drag the **▶** (the row under the brace) a beat to the right → the triangle moves, the audio, the warp markers and the ruler's "1" stay put; the stretch before ▶ is dimmed. Launch the clip → it starts a beat into the loop, goes to the end and then plays the whole loop from 1.1.1. Ctrl+Z → ▶ is back at 1.1.1.
2. Drag the **brace's body** (the coloured bar) a beat to the right → the loop moves along the clip, same length, ▶ stays; launch → the clip plays from ▶ into the loop and then goes round the loop only. Drag the brace's **left end** → the loop's start moves and its end stays (the Clip box's Position and Length follow); drag its **right end** or the **◀** → the length. Ctrl+Z each step.
3. Clip box → **Loop Off** → the brace fades; launch → the clip plays from ▶ to ◀ once and the slot stops. Put ▶ after ◀ → nothing plays.
4. Clip box: drag **Start** and **Position** (bars. beats. sixteenths) → ▶ and the brace move in the editor; type a value → the same. Save, reopen → both are back; a 0.22 set opens with every clip from 1.1.1 as before.
5. A MIDI clip: in the piano roll's ruler, the brace runs along the top and ▶ / ◀ sit under it. Drag ▶ to beat 2 → the clip plays from there (the playhead starts at beat 2 and goes round the loop); drag the brace's body, its ends, ◀ → as in the sample editor; drag the rest of the ruler up / down → it still zooms. The grid is shaded before the first marker and past ◀.
6. Right-click in the sample editor → **Set 1.1.1 Here** → the grid moves on the audio as before (▶ and the brace stay where they are on the grid); **Set End Here** → ◀ goes there (the length counts from the brace's start); select a stretch → **Loop Selection** → the brace is the selection from 1.1.1, ▶ at 1.1.1.
7. Drag a clip with ▶ at beat 2 from the Session onto the Arrangement → the placement starts at the clip's 1.1.1 (its offset is its own start marker) and loops the brace; split it → the right half carries on through the brace; a clip in the Arrangement with the brace away from 1.1.1 shows its loop seams where the brace repeats.
8. Click in the waveform before ▶ (a Session clip) → it plays from there and runs into the loop; click in a timeline clip's waveform → the start marker lands where that clip beat first plays.
9. Follow actions with time "Loop" on a clip with ▶ before the brace → the action fires when the first pass reaches ◀; Legato from a clip at beat 3 into a clip whose ▶ is at beat 1 → the new clip picks up at beat 3 of its own timeline.

## 21. The logo, About, and launching after a stop (0.24)

1. The app menu, the taskbar and the window show the new icon: the segmented S in amber-to-red on a charcoal tile. A `.sessions` file in the file manager carries it too.
2. Help → About → the Sessions logo in the theme's text colour over the new description; switch View → Theme to Light and open it again → the logo is dark on light.
3. Session View, song stopped at 1.1.1: launch a scene → every clip starts at once. Press the track's stop button on one track → it stops at the next bar. Press the transport **Stop** once → the song stops where it is (the position stays, say 9.1.1).
4. Now launch a single clip → it starts **at once** from 1.1.1 (before 0.24 it flashed and waited until the song got back to 9.1.1). Launch a scene from the same standstill → the same.
5. Click in the Arrangement at bar 5 (the start marker moves there) and, stopped, launch a clip → the song starts at bar 5 and the clip plays from its beginning right away.
6. With the song **playing**, launch another clip → it flashes and comes in on the next bar (the control bar's quantization; a clip with its own quantization in the Launch box uses that). Launch a scene while playing → every clip comes in together on the bar. Set the quantization to None → launches are immediate while playing too.

## 22. Your icon, Preferences with a sidebar, clean buttons (0.25)

1. Edit → Preferences (Ctrl+,) → a window with the categories down the left (Audio, Record, MIDI, Look, Startup) and the page on the right: a title, then rows with the setting's name on the left and its controls and a short note on the right. Click each category → its page; close and reopen → it opens on the one you were on. Resize it → the rows stretch, the sidebar stays.
2. Audio → the same settings as before (system, buffer, Outputs: Main out L / R, Inputs: Input 1 / 2, Look for interfaces again, Status). Change the buffer → it applies as before. PulseAudio → the output / buffer / input device rows instead.
3. Look → **App icon**: six icons with their names, the current one outlined. Click **Dusk** → the window's title bar / taskbar icon changes at once; Help → About → the logo's S is in Dusk's colours; the app menu shows the new icon (on some desktops after the next login; `~/.local/share/applications/sessions.desktop` now exists). Click **Ember** → the default again, the file is gone.
4. Each icon in turn, then About → the S follows (Playing: one orange clip, the rest in the text colour). Switch View → Theme to Light with Playing chosen → the grey clips are dark.
5. Dialog buttons: File → New Set with unsaved changes → Save / Discard / Cancel are plain buttons in the app's style, no desktop icons; the default one has the orange border; the question mark is a flat orange disc. The same in Preferences' Close, About's OK, the Zoom restart question, the theme editor's buttons, the export dialog.
6. Menus: File, Edit, View, Help → the panel colour, the orange highlight, greyed-out items (Undo with nothing to undo) read as disabled, separators inset. Right-click menus in the Session View, the Arrangement and the device chain look the same.

## 23. Instruments land anywhere, the zoom applies at once (0.26)

1. Browser → Instruments → drag **Monolith** over the Session View: over an empty MIDI track's column → the column outlines, drop → the instrument is on it (the Device View opens). Over a MIDI track that has an instrument → drop → it replaces the instrument, the clips stay. Over an **audio track with clips** → an orange bar with "New track" at the column's right edge, drop → a new MIDI track named Monolith right after it, holding Monolith; Ctrl+Z takes it away. Over the free space before the returns → "Drop Files, Samples and Devices Here" lights, drop → a new track at the end. Over the Master column → a new track too.
2. The same in the Arrangement: over a MIDI lane → the lane outlines; over an audio lane with clips → a line under it with "New track", drop → the new track under it; over the return lanes or under the Master → "New track" dashed at the bottom, drop → a new track at the end. The mixer strips in the Session View behave like their columns.
3. A Sound (Browser → Sounds), a MIDI effect (Arpeggiator) and a preset of yours do the same. Dragging a device *panel* by its title onto an audio track with clips is still refused (panels move by their own rules).
4. View → Zoom → 125 % → Sessions reloads at once at 125 %, the set as it was (unsaved changes included), the window where it was, the same view; only Ctrl+Z has nothing to undo. Zoom → 100 % → it reloads at 100 % right away (before 0.26 this needed a second, manual start). Preferences → Look → Zoom does the same.
5. With `QT_SCALE_FACTOR=1.25 sessions` from a terminal, View → Zoom reloads but your 1.25 stays: the environment wins.

## 24. Endless drags, typing into fields, dialog buttons that fit (0.28)

1. Control bar → drag the **BPM** field upwards past the top of the screen → the pointer jumps to the bottom of the screen and the tempo keeps rising as you go on dragging; the same downwards past the bottom. Shift still makes it fine. Every other field does it: mixer volume / pan / sends, Arrangement header fields, panel knobs, the Clip box, Monolith's mod amounts and its Position / ring displays.
2. Monolith → Mod tab → drag a route's amount up and down → it follows smoothly; double-click it → a plain field with **35** (not 0.35) and nothing showing through; type 40 → 40 %; type "-20 %" → -20 %. Double-click the tempo → type 128 → 128.
3. File → New Set with unsaved changes → the three buttons read in full: **Save**, **Close without Saving** (or Discard), **Cancel**; nothing cropped, the default outlined in orange. Also check Export Audio (Export… / Cancel), Keyboard Shortcuts (OK), View → Theme → Customize… (Save / Delete / Revert), Preferences (Close), About (OK).
4. File → Export Audio → **Tail** is a drag field: drag it, double-click to type 5 → "5 s".

## How to tell me what you found

For each problem: the section and number (e.g. "4.3"), what you did, what happened, and what you expected. For sound problems, which device and settings, and whether it's a click, crackle, noise, distortion, a wrong level or a delay. If something crashed, say what you clicked last.
