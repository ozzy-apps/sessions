# Release process

The public repository is `ozzy-apps/sessions`. Application updates use published, non-draft, non-prerelease GitHub Releases. Keep sample-pack releases separate from the latest stable app release.

## Build and review

1. Reconcile current main and parallel work; retain newer versions and all documentation. Choose a version in `CMakeLists.txt`, update AppStream metadata/release notes, and choose the project's software license before publication.
2. Run the full build and regression suites, inspect actual screenshots and package metadata, and test audio/MIDI on supported hardware.
3. Build the four formats. `.deb` is automatic. For AppImage, `scripts/fetch-appimage-tools.py` verifies reviewed SHA-256 values in `distribution/appimage-tools.json`, then `scripts/package-appimage.sh` uses `LINUXDEPLOY` and `LINUXDEPLOY_QT`. Review/update the lock when upstream continuous tools change. Flatpak uses `scripts/package-flatpak.sh` with KDE Platform/SDK 6.10 installed. Windows uses `scripts/package-windows.sh` in MSYS2 UCRT64.
4. Retain shared-library license texts with portable distributions; [third-party notices](../distribution/THIRD-PARTY.md) supplement those licenses. Windows releases also need a real Windows audio/MIDI check. The initial ZIP is unsigned; Windows signing needs the owner's signing identity, and is separate from compiling a working executable.

## GitHub Actions

`build.yml` runs Linux tests and a Windows build/short offline render on main/PRs. `release.yml` packages all formats after a `vX.Y.Z` tag and creates a **draft** release only after both platforms succeed. Manual dispatch builds artifacts without publishing a release. Tags must match the CMake version; the tagged release gate requires a `LICENSE` file.

These are publishing actions; this repository's instructions require explicit user authorization before pushing or tagging. Once authorized, push the reviewed source and matching tag normally. Review the draft assets, run the hardware checks, then publish the draft. No public release is created merely by building locally.

Expected assets:

| Asset | Purpose |
| --- | --- |
| `sessions-linux-amd64.deb` | Debian/Ubuntu x64 installer |
| `Sessions-linux-x86_64.AppImage` | Portable Linux x64 application |
| `Sessions-linux-x86_64.flatpak` | Flatpak bundle; install with `flatpak install --user FILE` |
| `Sessions-windows-x86_64.zip` | Portable Windows x64 application |
| `SHA256SUMS` | SHA-256 of every app asset |

The updater validates the version and GitHub URLs, selects the matching format/architecture, and falls back to the release page if that format is absent. Source builds also use the release page. It never installs, executes or replaces a running app. Download opens the browser, Postpone suppresses that version for 24 hours, and Skip suppresses that version until a later release. Manual checks override skips/postponement. Disabling startup checks does not disable the manual action. Offline/API failures stay quiet at startup; manual checks show a status.

The request goes to GitHub's release API with the app version in the User-Agent; it sends no project, audio or hardware data. Opening Sample Packs also requests the public catalog. Both use ordinary HTTPS; certificate errors are not bypassed.

## Drum collection release

Build the ZIP with `python3 scripts/package-drums.py`. Upload the archive and `.sha256` to a separate `drum-content-v1` release, including the included licensing/provenance. Use **prerelease** and **not latest** for content-only releases. The catalog/README use that stable URL; neither is a live download until the asset is uploaded. Later packs get new independent tags and catalog entries.

References: [GitHub Releases API](https://docs.github.com/en/rest/releases/releases), [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html), [Flatpak building and bundles](https://docs.flatpak.org/en/latest/first-build.html), [linuxdeploy](https://docs.appimage.org/packaging-guide/from-source/linuxdeploy-user-guide.html).
