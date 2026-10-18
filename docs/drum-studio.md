# Drum Studio · Sessions 0.30

Load **Browser → Drums → Drum Studio**. Click **Expand** to work inside Sessions’ device pane. The upper Session/Arrangement stays visible; drag the divider to resize the editor. **Device chain ▾** collapses it. Osmium uses the same host, with Sound, Modulators, Voice & mixer and Effects pages. Device surfaces use Sessions’ gray panels, displays, accent and theme settings.

![Drum Studio expanded inside Sessions](screenshots/drum-studio/sessions-drums-expanded.png)

## Recorded kits

| Recipe | Recording | Character |
| --- | --- | --- |
| Studio Rock | Black Pearl | Balanced Pearl kit |
| Vintage | Red Zeppelin | Low-tuned Ludwig kit |
| Tight Pop | Black Pearl | Shorter, brighter articulation |
| Metal | Red Zeppelin | Defined attack and damped shells |
| Jazz | Blonde Bop | Smaller Tama kit, longer natural tails |
| Big Room | Red Zeppelin | Natural large-kit voicing; raise Room for space |

The kit recipe selector preserves the six existing saved choices. All original 50 parameter indices remain; eight mutes and eight solos are appended. Existing sets acquire these switches turned off.

285 unmodified mono recordings provide five velocity layers for each of 19 articulations in three kits. GM kick 35/36, snare 38/40, side stick 37, clap 39, hats 42/44/46, toms 41/43/45/47/48/50, crashes 49/57, china 52, splash 55, ride 51/59 and bell 53 are mapped. The high tom uses the recorded rack tom tuned up five semitones. Velocity layers are separately recorded strikes, not round robins. Recordings are shared between instances; file reads happen when preparing the processor, outside audio rendering. Playback uses 64 voices and supports the output sample rate independently of the sources’ 44.1kHz rate.

**Kit:** click a drum, then choose an articulation. The waveform displays the recorded source; **Audition** uses the adjustable velocity. Level / Pan / Tune / Decay / Tone apply per drum. Wires and Beater shape snare/kick attack. Decay damps the tail below ×1 and emphasizes the recorded sustain above ×1, without changing pitch. Repeated shells overlap; each hat strike closes previous hat voices with a short fade. All Off fades active hits.

**Mixer:** eight faders, Pan, Mute / Solo and an ambience/output section. Mute wins over Solo. Overheads, Room, Room Size, Bleed and Width model extra ambience from the recordings; these are not independently recorded microphone channels. The kit samples themselves retain their original recording sound. This is a compact recorded kit instrument, with neither a multi-gigabyte mic library nor proprietary drum samples.

## Grooves and clips

92 one/two-bar grooves in 19 categories: Rock, Pop, Funk, Hip Hop, House, Drum & Bass, Metal, Jazz, Reggae, Latin, Blues, Punk, R&B, Fills, Techno, Breakbeat, Disco, Afrobeat and Dubstep. Patterns include backbeats, ghost-note breaks, double kicks/blasts, one drop, steppers, shuffles, dance patterns and fills. They are original programmed patterns, not transcriptions or sampled break recordings.

**Grooves:** choose a category and/or search a name. The grid shows hit velocity and placement. **Preview** loops at the set tempo, without inserting a clip or starting transport. Preview stops when leaving the page, collapsing or changing tracks. Velocity/Timing adjust deterministic humanization, shared by preview, clip creation, drag/drop and MIDI export. The loop length is one/two bars at 4/4.

- Drag a groove onto a Session slot or Arrangement lane. MIDI tracks receive the clip; dropping onto an audio track creates a MIDI track with Drum Studio.
- **To Clip** or double-click inserts an editable MIDI clip in this track’s next empty slot, in one undo step. Play it at the set tempo and edit it in the normal piano roll.
- **Export MIDI…** writes SMF type 0, 960 PPQN, GM MIDI channel 10, tempo metadata, a 4/4 time signature and exact end-of-loop placement. Every note-on has a note-off. Exports use the groove’s suggested tempo; Sessions clips follow the set tempo.
- The release ZIP contains all 92 `.mid` files and 92 `.sessionsclip` files, grouped by style. Drag native clips into Sessions or copy them into the Clips folder shown by User Library. MIDI files can be opened by other DAWs.

## Sources and reproduction

Samples: **Glen MacArthur, AVL Drumkits / Bandshed Records**, from [the author’s collection](https://www.bandshed.net/avldrumkits/). The selected/remapped library is named **Sessions Acoustic Collection** and carries CC-BY-SA 3.0 with the author’s exception allowing produced music to be freely licensed. Attribution and complete license notices are included in [assets/drums/README.md](../assets/drums/README.md), its PDF and legal code; these files ship in the package. This separate sample license does not change the application’s code license.

`scripts/fetch-drum-samples.py` reproduces the bundled unmodified files from checksum-pinned source archives. Normal builds and playback require no downloads. CMake installs the recordings under `share/sessions/drums`; build-tree runs use the bundled source directory. The library occupies about 67MB on disk and 134MB as shared decoded mono audio.

```sh
QT_QPA_PLATFORM=offscreen ./build/sessions_drum_studio /tmp/drum-studio
```

This runs the recorded-layer, GM map, audio headroom/dynamics/choke, groove timing, SMF parser, native-clip round-trip and embedded UI checks; optionally writes the two clip formats, a multi-style audition WAV, guide and screenshots. Automated checks verify audio behavior and bounds; use the manual checklist to audition on your playback system.
