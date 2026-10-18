# Osmium editing · 0.31.1

Osmium uses the current Sessions theme and remains embedded in the device pane. Drag the pane divider for more editing space. Sound sections reflow at narrower widths, with scrolling when needed.

Every dial now has a 34px minimum center; macros have 54px centers. The inner ring shows the base value. The outer ring uses the theme accent: a faded track shows the whole available sweep and the bright part shows the modulation range. A gap separates the two controls. Drag the center or inner ring vertically for the base; drag the outer ring up/right to increase modulation, down/left to decrease or invert it. Shift gives finer control. The gap and label do not start gestures. Click the source slot or drop a source to assign it; the range inspector accepts exact signed amounts.

Both phrase curves support 257 nodes. Choose **X: 1/4** through **X: 1/256**, or **Free**. The X grid controls node snap and freehand resolution; Free draws 256 intervals. A fast Draw stroke interpolates skipped positions and creates one undo step. Drawing resamples the existing shape at the chosen resolution, so choose a fine grid before painting detailed curves. **Y** optionally snaps levels independently. Alt bypasses node snapping.

Turn Draw off to edit nodes. Double-click empty space to add one, double-click an interior node to remove it, or select nodes and press Delete. Endpoints retain their time. Drag a diamond to bend its segment, or use exact **X**, **Y** and **Bend** percentages below the graph. **Curve**, **Smooth** and **Hold** affect the selected outgoing segment. Bend converts a held segment back to Curve.

Shift-click toggles nodes; dragging empty space selects a box. Selected nodes move together while preserving their spacing and level offsets, stopping at their neighbors and the canvas limits. Ctrl-drag locks the dominant axis. Arrow keys nudge time by the X grid and level by the Y grid (or 1%); Shift uses 1/4096 time and 0.1% level. Ctrl+A selects all, Escape clears selection. Tab / Shift+Tab cycle selected nodes; Escape then Tab leaves the graph.

**1×–8×** zoom magnifies time. The scrollbar reaches the rest of the curve; zoom does not alter saved timing. Dense grids hide lines closer than five pixels until zoomed in. Invert, Reverse, Copy and Paste retain node/segment data. Envelope, LFO, curve and step displays grow with the pane.

Dense curves round-trip in sets and user presets and use preallocated render storage with logarithmic segment lookup. Monolith retains its original 32-node limit and parameter layout. **Builds without these changes only read 32 nodes**. Reopen dense projects using a build that includes this branch.

Developed on `osmium-usability`, based on 0.31.0, and integrated into `main` at the user’s request. Version 0.31.1 avoids colliding with the separate sampler worktree’s 0.32.0. When integrating the sampler later, retain the newer release version.
