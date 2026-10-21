# Source tempo and synchronized stems

Warp needs the tempo of the recording, separately from the project's tempo. A wrong source estimate changes playback speed: reading a 174 BPM recording as 116 BPM makes it play about 1.52 times faster in a 176 BPM project. An unknown estimate (`—`) leaves Warp off and plays at recorded speed.

## Import

**Preferences → Record / Warp → Default Warp Mode** sets the algorithm for new clips. Existing clips retain their saved choice. Mode-specific settings can be prepared with Warp off. Pitch and detune then change playback speed and pitch together; the project tempo does not set their playback rate.

Import synchronized exports together. Long files from the same folder with matching durations share one rhythmic reference and the same timing map. The reference normally comes from drums or percussion; a keyboard or sustained synth does not need to reveal a beat independently.

When importing one file at a time, recognized stem names such as `0 Drums`, `1 Bass`, `Keyboard`, `Synth` and `Vocals` can join already imported stems from the same export folder and duration. Sessions does not scan or decode unrequested neighboring recordings. Short loops, different folders and different durations remain independent. Manual source settings, metadata and edited maps are protected from automatic changes. Conflicting declared tempos do not establish a shared reference.

If the group has no reliable tempo evidence, its automatically analyzed members stay at recorded speed together. The project tempo is not silently substituted for an unknown source tempo.

## Repair existing clips

Right-click **Source BPM**, **Sync**, or the waveform:

- **Re-detect Tempo** analyzes the selected recording again, replacing its saved estimate and timing map.
- **Re-detect Matching Stems** analyzes the related recordings together and applies one shared map. This is the repair for an older set whose stems acquired different automatic estimates.
- **Use Project Tempo for Matching Stems** uses the project BPM as a known source tempo for the related files. Without related files, **Use Project Tempo** affects the selected clip.
- **Apply Source Tempo to Matching Stems** shares the selected source timing with its related files.

Each repair is undoable. Re-detection is an explicit replacement of the timing map; loading an older set alone preserves its saved markers. Ordinary **Sync** retains the source reference rather than independently guessing again.

## Changing source tempo

Some generated or recorded tracks change tempo. Sessions tracks broad, supported tempo changes from the rhythmic reference and applies the resulting map to every stem. It preserves the original file start and silence before an instrument enters.

The tempo line shows a source range when the map varies. **Source BPM** is then the map's average; editing it scales the map while retaining its tempo changes. The project tempo remains the playback destination. Automatic analysis is conservative: weak or ambiguous passages cannot establish a new beat count by themselves.

A generation prompt's BPM is useful context, but measure the exported audio before treating it as exact. The supplied examples illustrate this: one is steady near 174 BPM, while the other accelerates from roughly 174 toward 181–183 BPM.
