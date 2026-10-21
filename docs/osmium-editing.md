# Osmium editing · 0.39.0

The compact Osmium panel also has a searchable **preset selector**, **previous** and **next** buttons (0.43). The arrows wrap through the original starting sounds and all 112 factory presets, skipping the separator. Selection replaces parameters, routes and curves in one undo step and follows undo/redo in both panel sizes. The 24 [Gesture Bass presets](osmium-gesture-bass.md) use asymmetric one-shot contours with held endpoints for bass movement without repeating LFO rhythms.

Osmium uses the current Sessions theme and remains embedded in the device pane. Drag the pane divider for more editing space. Sound sections reflow at narrower widths, with scrolling when needed.

Position and cutoff use larger 64px dials; ordinary controls use 52px dials, and macros retain 84px dials with 54px centers. The inner ring shows the base value. The outer ring uses the theme accent: a faded track shows the whole available sweep and the bright part shows the modulation range. A gap separates the two controls. Drag the center or inner ring vertically for the base; drag the outer ring up/right to increase modulation, down/left to decrease or invert it. Shift gives finer control. The gap and label do not start gestures. Click the source slot or drop a source to assign it; the range inspector accepts exact signed amounts.

The Sound page gives each oscillator an expanding display. **Table** shows all eight morph frames with the active position highlighted; **Wave** shows the shaped cycle; **Harm** shows that cycle's harmonic content. Drag horizontally to scan Position, use Shift for finer movement, or use arrows/the wheel. One drag is one undo step. The table browser searches both names and families and previews actual table frames. Complete oscillator/filter cards wrap at narrower widths; controls remain accessible through scrolling.

Each oscillator offers **Bend, Formant, Sync, Phase, Fold, Asym, Steps** and **Cross FM**. The Warp dial sets depth. Cross FM uses the next oscillator (2→1, 3→2, 1→3); the source's table, scan position and tuning matter even at zero audible level. The independent **Fold** dial adds wavefolding after the table read and accepts ordinary modulation. Folding uses antiderivative antialiasing and removes DC; Steps deliberately supplies digital phase quantization. All added controls default to Legacy/bypass so existing sounds and automation retain their meaning.

The two filter displays support direct cutoff/resonance dragging and expand with the Sound page. Voice controls are grouped by function. Effects show setting-based responses alongside their controls. The editor follows the theme and retains large performance macros and separate base/range hit areas.

Both phrase curves support 257 nodes. Choose **X: 1/4** through **X: 1/256**, or **Free**. The X grid controls node snap and freehand resolution; Free draws 256 intervals. A fast Draw stroke interpolates skipped positions and creates one undo step. Drawing resamples the existing shape at the chosen resolution, so choose a fine grid before painting detailed curves. **Y** optionally snaps levels independently. Alt bypasses node snapping.

Turn Draw off to edit nodes. Double-click empty space to add one, double-click an interior node to remove it, or select nodes and press Delete. Endpoints retain their time. Drag a diamond to bend its segment, or use exact **X**, **Y** and **Bend** percentages below the graph. **Curve**, **Smooth** and **Hold** affect the selected outgoing segment. Bend converts a held segment back to Curve.

Shift-click toggles nodes; dragging empty space selects a box. Selected nodes move together while preserving their spacing and level offsets, stopping at their neighbors and the canvas limits. Ctrl-drag locks the dominant axis. Arrow keys nudge time by the X grid and level by the Y grid (or 1%); Shift uses 1/4096 time and 0.1% level. Ctrl+A selects all, Escape clears selection. Tab / Shift+Tab cycle selected nodes; Escape then Tab leaves the graph.

**1×–8×** zoom magnifies time. The scrollbar reaches the rest of the curve; zoom does not alter saved timing. Dense grids hide lines closer than five pixels until zoomed in. Invert, Reverse, Copy and Paste retain node/segment data. Envelope, LFO, curve and step displays grow with the pane.

Dense curves round-trip in sets and user presets and use preallocated render storage with logarithmic segment lookup. Monolith retains its original 32-node limit and parameter layout. **Builds without these changes only read 32 nodes**. Reopen dense projects using a build that includes this branch.

Developed on `osmium-usability`, based on 0.31.0, and integrated into `main` at the user’s request. Version 0.31.1 avoids colliding with the separate sampler worktree’s 0.32.0. When integrating the sampler later, retain the newer release version.
