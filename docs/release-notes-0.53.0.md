# Sessions 0.53.0

- **Stems Creator:** load a mixed file and separate it locally into Vocals, Drums, Bass, Piano and Other. Five aligned audio tracks arrive inside one group, with one Undo step and the original preserved.
- CPU thread selection, progress, cancellation and output/model folder options. Models and the native CPU runtime are bundled; no Python or audio upload is required.
- **Help → Check for Updates** now responds reliably when a startup check is already running. Manual requests share that request and override skip/postpone preferences.

Separation is model-dependent and can contain bleed. Guitar and remaining instruments share Other. Output is 44.1 kHz stereo float WAV; files up to one hour are accepted, subject to available memory. See the [device guide](stems-creator.md).

Windows Stems Creator requires the [Microsoft Visual C++ x64 Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). Physical Windows audio/MIDI verification remains outstanding.

[Downloads and release assets](https://github.com/ozzy-apps/sessions/releases/tag/v0.53.0).
