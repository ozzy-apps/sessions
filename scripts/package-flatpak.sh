#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${1:-build/flatpak}"
repo_dir="${2:-build/flatpak-repo}"
flatpak-builder --force-clean --jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" --repo="$repo_dir" "$build_dir" distribution/flatpak/nl.co.ozzy.Sessions.json
mkdir -p dist
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo "$repo_dir" dist/Sessions-linux-x86_64.flatpak nl.co.ozzy.Sessions
