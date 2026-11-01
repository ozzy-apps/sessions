Sessions 0.53.0 adds **Stems Creator** and improves update checking.

- **Separate a mix locally:** load an audio file in **Audio Effects → Stems Creator**, then choose **Create Stems**. Sessions creates Vocals, Drums, Bass, Piano and Other as five aligned Arrangement tracks inside one group.
- **Keep control:** choose CPU threads and an output folder, follow progress or cancel. The original stays intact, and the grouped import has one Undo step. Models and the native CPU runtime are bundled; no Python, account or audio upload is required.
- **Check for Updates:** the Help menu has the explicit action. Manual checks now work when a startup check is already running and override skipped/postponed versions.

Separation can contain bleed and artifacts. Guitar and remaining instruments share **Other**; this model does not isolate every instrument individually. Output is 44.1 kHz stereo float WAV. Files up to one hour are accepted, subject to available memory. [Stems Creator guide](https://github.com/ozzy-apps/sessions/blob/v0.53.0/docs/stems-creator.md).

Choose the `.deb`, AppImage, Flatpak bundle or Windows portable ZIP. `SHA256SUMS` covers all four downloads. Save your work and close Sessions before installing an update.

**Windows:** Stems Creator requires the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). Windows was checked under Wine; physical Windows audio/MIDI testing remains outstanding.

**Validation:** all 37 native CTest suites passed. Real neural separation was verified in the Debian, AppImage, Flatpak and Windows environments, with grouped import, timing, undo/redo, persistence and cancellation checks. Live HTTPS update checks passed using native, AppImage, Flatpak and Windows Qt/TLS environments.

The optional [Sessions Drum Collection](https://github.com/ozzy-apps/sessions/releases/tag/drum-content-v1) remains a separate download.
