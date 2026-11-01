Sessions 0.52.0 adds three recording and Arrangement workflows:

- **Capture MIDI** — recall the selected track's last played phrase without having enabled Record. Use **Create → Capture MIDI** or **Ctrl+Shift+C**. Capture uses a free Session slot or an Arrangement placement, with undo/redo.
- **Bounce Track to Audio** — print an Arrangement track, group or selected range to a new audio track. The original remains editable; source activation and the new track share one undo step.
- **Aligned stem export** — export individual tracks, groups and returns over the same range, with matching sample lengths and a timing manifest.

Rendering keeps sidechains running, warms earlier effects before a selected range, and lets effects ring after the range without admitting later clips. Normalization streams through disk instead of holding the whole song in memory; 32-bit float WAV files retain headroom above 0 dBFS.

Bounce is currently Arrangement-only and post-mixer. Return effects are separate, and this update does not provide in-place bounce or a processor-suspending Freeze system. [Workflow guide](https://github.com/ozzy-apps/sessions/blob/v0.52.0/docs/recording-workflow.md) · [Feature comparison](https://github.com/ozzy-apps/sessions/blob/v0.52.0/docs/ableton-parity.md).

Choose the `.deb`, AppImage, Flatpak bundle or Windows portable ZIP. `SHA256SUMS` lists all four downloads. Save your work and close Sessions before installing an update. The optional [Drum Collection](https://github.com/ozzy-apps/sessions/releases/tag/drum-content-v1) remains a separate download.

Validation: all 36 Linux CTest suites passed, including 69 new recording checks. Those 69 checks also passed under Wine, and the AppImage, Flatpak and Windows builds each rendered a two-second stereo WAV. Windows remains an early build pending physical audio/MIDI testing; ASIO and plugin hosting are not included.
