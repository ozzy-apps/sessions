# Sample packs

The application includes the original three AVL drum kits. The optional **Sessions Drum Collection** adds six libraries and 6,061 recordings; it is distributed separately so application updates stay small.

[Download the cross-platform ZIP](https://github.com/ozzy-apps/sessions/releases/download/drum-content-v1/sessions-drum-content-v1.zip). This URL becomes live when the `drum-content-v1` release is published. The existing Debian-only drum-content installer remains usable on Debian/Ubuntu.

1. Download and extract the ZIP to a folder you intend to keep.
2. Open **Help → Sample Packs → Locate Drum Studio content**.
3. Select the extracted `drum-content` folder, containing `library.json` and `audio`.
4. Choose a library in Drum Studio. Content is loaded in the background, with long recordings streamed from disk.

Moving the pack later requires locating it again. For ordinary one-shots and loops, add their folder through **Browser → Places**. Sample packs retain their own licenses; exact attribution/provenance ships under `drum-content/licenses`.

## Adding another pack

Add an entry to [distribution/content-packs.json](../distribution/content-packs.json), using a stable `id`, display `name`, `version`, plain-text `description` and HTTPS download `url`. The in-app list refreshes from that public catalog when opened, with the bundled catalog as its offline fallback. Packs can live in independent GitHub Releases, so publishing samples does not require making a new application version.

The catalog lists downloads; it does not run installers or execute pack contents. The current Drum Studio format has six stable bank indices. Adding new Drum Studio banks requires extending its model/choices while preserving those indices. New ordinary sample collections can already be distributed through the catalog and loaded through Browser → Places.

## Building the existing pack

Fetch/prepare the source recordings using the existing scripts, then:

```sh
python3 scripts/package-drums.py
```

The script writes `dist/sessions-drum-content-v1.zip` and its SHA-256 sidecar, including the manifest, audio, license texts and provenance. It does not recompress or modify the FLAC recordings. Keep this pack's release a **prerelease** or mark it **not latest**, so it never replaces the stable application release used by the updater. See [releasing](releasing.md).
