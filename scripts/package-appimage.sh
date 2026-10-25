#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${1:-build}"
tools_dir="${SESSIONS_APPIMAGE_TOOLS:-$PWD/build/appimage-tools}"
app_dir="$PWD/build/AppDir"
cmake -E rm -rf "$app_dir"
mkdir -p "$tools_dir" dist
# Explicit tool inputs permit pinned, independently verified release downloads.
: "${LINUXDEPLOY:?Set LINUXDEPLOY to a verified linuxdeploy AppImage}"
: "${LINUXDEPLOY_QT:?Set LINUXDEPLOY_QT to a verified linuxdeploy-plugin-qt AppImage}"
cp "$LINUXDEPLOY" "$tools_dir/linuxdeploy-x86_64.AppImage"
cp "$LINUXDEPLOY_QT" "$tools_dir/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x "$tools_dir/"*.AppImage
cmake --install "$build_dir" --prefix "$app_dir/usr" --component Runtime
export QMAKE="${QMAKE:-$(command -v qmake6)}"
export OUTPUT="$PWD/dist/Sessions-linux-x86_64.AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1
export EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS:-libqoffscreen.so}"
# libsndfile is loaded dynamically, so it must be included explicitly.
sndfile="$(ldconfig -p | awk '/libsndfile.so.1 .*x86-64/ {print $NF; exit}')"
test -n "$sndfile"
"$tools_dir/linuxdeploy-x86_64.AppImage" --appdir "$app_dir" --plugin qt --library "$sndfile" --output appimage
